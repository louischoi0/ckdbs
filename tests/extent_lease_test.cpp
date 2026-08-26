#include "kds/storage/extent_lease.hpp"

#include <array>
#include <set>
#include <span>

#include <gtest/gtest.h>

#include "kds/storage/free_map.hpp"

// Page-id extents (docs/inflight/in-progress/workplan-crosscore.md M5/P5). The property that
// matters is **exclusivity**: an id promised to one core must never be found
// free by another, which is why a reservation marks the free map rather than
// merely remembering a range.

namespace kds::storage {
namespace {

using MapPage = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> AsSpan(MapPage& p) {
    return std::span<std::byte, kPageSize>(p);
}

class ExtentAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override { FormatFreeMapPage(AsSpan(map_)); }

    MapPage map_{};
};

TEST_F(ExtentAllocatorTest, AReservationIsContiguousAndMarkedAllocated) {
    ExtentAllocator alloc(AsSpan(map_), /*first_new_page_id=*/128);

    auto e = alloc.Reserve(64);
    ASSERT_TRUE(e.ok()) << e.status().message();
    EXPECT_EQ(e.value().first, 128u);
    EXPECT_EQ(e.value().count, 64u);
    EXPECT_EQ(e.value().end(), 192u);

    // Marked at reservation, not at first use: an unspent lease has to look
    // allocated, or another core's CreateNew() would find the same ids.
    for (PageId id = 128; id < 192; ++id) {
        EXPECT_TRUE(FreeMapIsAllocated(AsSpan(map_), id)) << "id " << id << " left free";
    }
    EXPECT_FALSE(FreeMapIsAllocated(AsSpan(map_), 192));
}

TEST_F(ExtentAllocatorTest, SuccessiveReservationsAreDisjoint) {
    ExtentAllocator alloc(AsSpan(map_), 128);

    std::set<PageId> seen;
    for (int i = 0; i < 8; ++i) {
        auto e = alloc.Reserve(16);
        ASSERT_TRUE(e.ok()) << e.status().message();
        for (PageId id = e.value().first; id < e.value().end(); ++id) {
            EXPECT_TRUE(seen.insert(id).second) << "id " << id << " was issued twice";
        }
    }
    EXPECT_EQ(seen.size(), 8u * 16u);
    EXPECT_EQ(alloc.reservations(), 8u);
}

TEST_F(ExtentAllocatorTest, AReservationSkipsPagesSomeoneElseAllocated) {
    // The interleaving that matters: core 0 allocates a page in the middle
    // of where the next lease would have gone, so the run has to move past
    // it rather than straddle it.
    ExtentAllocator alloc(AsSpan(map_), 128);
    FreeMapAllocate(AsSpan(map_), 130);

    auto e = alloc.Reserve(8);
    ASSERT_TRUE(e.ok()) << e.status().message();
    EXPECT_FALSE(e.value().Contains(130)) << "the run straddled an allocated page";
    EXPECT_EQ(e.value().first, 131u);
}

TEST_F(ExtentAllocatorTest, AZeroPageReservationIsRefused) {
    ExtentAllocator alloc(AsSpan(map_), 128);
    EXPECT_EQ(alloc.Reserve(0).status().code(), StatusCode::kInvalidArgument);
}

TEST_F(ExtentAllocatorTest, ARunThatWouldLeaveThisAllocatorsOnePageIsOutOfSpace) {
    // **A page's coverage, not the instance ceiling.** It was both until the
    // free map became multi-page: over a store the search would now cross
    // into region 1 and create it (FM5), and the instance ceiling is
    // kMaxPageCount. The bare-bytes form holds exactly one page, so region 0
    // is all it has and running past it is still OutOfSpace - which is what
    // this test now pins.
    ExtentAllocator alloc(AsSpan(map_), kFreeMapBitsPerPage - 4);

    auto e = alloc.Reserve(64);
    ASSERT_FALSE(e.ok());
    EXPECT_EQ(e.status().code(), StatusCode::kOutOfSpace);
    EXPECT_NE(e.status().message().find(std::to_string(kFreeMapBitsPerPage)), std::string::npos)
        << e.status().message();
}

TEST_F(ExtentAllocatorTest, ARunLongerThanARegionCanNeverBePlaced) {
    // D3(a) refuses to straddle, so a region is the longest possible
    // reservation - reported up front rather than after a walk of the whole
    // id space that could not have succeeded.
    ExtentAllocator alloc(AsSpan(map_), 128);
    auto e = alloc.Reserve(kFreeMapBitsPerPage + 1);
    ASSERT_FALSE(e.ok());
    EXPECT_EQ(e.status().code(), StatusCode::kOutOfSpace);
}

// ---- LeasedIdSource ---------------------------------------------------

TEST(LeasedIdSourceTest, IdsComeOutInOrderAndThenRunOut) {
    LeasedIdSource lease(Extent{500, 3});

    for (PageId expected : {500u, 501u, 502u}) {
        auto id = lease.Next();
        ASSERT_TRUE(id.ok()) << id.status().message();
        EXPECT_EQ(id.value(), expected);
    }

    // TxnConflict - the one retryable code - and deliberately not
    // OutOfSpace: the device may have plenty of room and this core simply
    // has no ids in hand. Conflating the two would turn "ask core 0 for
    // more" into "the database is full".
    auto spent = lease.Next();
    ASSERT_FALSE(spent.ok());
    EXPECT_EQ(spent.status().code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(spent.status().retryable());
    EXPECT_TRUE(lease.spent());
}

TEST(LeasedIdSourceTest, ADefaultConstructedSourceHasNothingToGive) {
    LeasedIdSource lease;
    EXPECT_TRUE(lease.spent());
    EXPECT_EQ(lease.Next().status().code(), StatusCode::kTxnConflict);
}

TEST(LeasedIdSourceTest, AGrantResumesIssuingAndBurnsTheOldRemainder) {
    LeasedIdSource lease(Extent{100, 4});
    ASSERT_TRUE(lease.Next().ok());  // 100

    lease.Grant(Extent{700, 2});
    auto id = lease.Next();
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id.value(), 700u) << "issuing did not move to the new extent";

    // 101..103 are burned. Ids are unique and monotonic per core, never
    // gapless - the same trade TrxIdSequence makes.
    EXPECT_EQ(lease.remaining(), 1u);
}

TEST(LeasedIdSourceTest, PagesFromASpentLeaseAreStillOwned) {
    // The bug this exists to prevent: forget the old extent and a core's
    // ownership check rejects its own pages the moment it takes a second
    // lease - reported as somebody else's fault.
    LeasedIdSource lease(Extent{100, 4});
    lease.Grant(Extent{700, 4});

    EXPECT_TRUE(lease.Owns(100)) << "a page from the previous lease was disowned";
    EXPECT_TRUE(lease.Owns(103));
    EXPECT_TRUE(lease.Owns(700));
    EXPECT_FALSE(lease.Owns(104));
    EXPECT_FALSE(lease.Owns(699));
}

TEST(LeasedIdSourceTest, AContiguousGrantMergesInsteadOfAccumulating) {
    // The ordinary case - the allocator carves sequentially - and the reason
    // Owns() can afford a linear scan.
    LeasedIdSource lease(Extent{100, 64});
    lease.Grant(Extent{164, 64});
    lease.Grant(Extent{228, 64});

    ASSERT_EQ(lease.granted().size(), 1u);
    EXPECT_EQ(lease.granted()[0].first, 100u);
    EXPECT_EQ(lease.granted()[0].count, 192u);
    EXPECT_TRUE(lease.Owns(291));
    EXPECT_FALSE(lease.Owns(292));
}

TEST(LeasedIdSourceTest, ADiscontiguousGrantIsKeptSeparately) {
    LeasedIdSource lease(Extent{100, 4});
    lease.Grant(Extent{900, 4});

    EXPECT_EQ(lease.granted().size(), 2u);
    EXPECT_FALSE(lease.Owns(500));
}

TEST(LeasedIdSourceTest, LowWaterFiresWhileThereAreStillIdsToHandOut) {
    // The whole point of asking early: the grant has to arrive while the
    // core can still allocate, or a statement fails for want of a message.
    LeasedIdSource lease(Extent{100, 8});
    EXPECT_FALSE(lease.low_water());

    for (int i = 0; i < 6; ++i) ASSERT_TRUE(lease.Next().ok());
    EXPECT_TRUE(lease.low_water());
    EXPECT_GT(lease.remaining(), 0u) << "the refill signal came too late to be useful";
}

// ---- The two together -------------------------------------------------

TEST_F(ExtentAllocatorTest, TwoCoresLeasingFromOneMapNeverShareAnId) {
    // The property the whole mechanism exists for.
    ExtentAllocator alloc(AsSpan(map_), 128);

    LeasedIdSource core1;
    LeasedIdSource core2;
    for (int round = 0; round < 4; ++round) {
        auto a = alloc.Reserve(8);
        ASSERT_TRUE(a.ok());
        core1.Grant(a.value());
        auto b = alloc.Reserve(8);
        ASSERT_TRUE(b.ok());
        core2.Grant(b.value());
    }

    std::set<PageId> from1;
    while (!core1.spent()) from1.insert(core1.Next().value());
    while (!core2.spent()) {
        const PageId id = core2.Next().value();
        EXPECT_EQ(from1.count(id), 0u) << "id " << id << " was issued to both cores";
        EXPECT_FALSE(core1.Owns(id)) << "id " << id << " is claimed by both cores";
    }
}

}  // namespace
}  // namespace kds::storage
