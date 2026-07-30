#include "kds/stats/waystone_hooks.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The coverage hooks (waystone-concpets.md section 9, spec test 12-4's
// hook half). Coverage is the only guarantee Waystone makes, so what these
// tests are really checking is that it is *unconditional*: after an
// insert the entry exists and is live, after a delete it is not live, and
// neither call ever needs to know anything about the relation beyond its
// directory.

namespace kds::stats {
namespace {

class WaystoneHooksTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto root = CreateDirPage(store_);
        ASSERT_TRUE(root.ok()) << root.status().message();
        ws_.dir_root = root.value();
        ws_.depth = 1;
    }

    storage::InMemoryPageStore store_{1};
    WaystoneRef ws_;
};

// ---- Coverage -----------------------------------------------------------

TEST_F(WaystoneHooksTest, AnInsertedTupleHasALiveEntryNamingWhereItWent) {
    ASSERT_TRUE(OnInsert(store_, ws_, /*pk=*/42, /*page_id=*/700, /*slot=*/3, /*epoch=*/5).ok());

    auto entry = LookupEntry(store_, ws_, 42);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_EQ(entry.value().pk, 42u);
    EXPECT_EQ(entry.value().page_id, 700u);
    EXPECT_EQ(entry.value().slot, 3);
    EXPECT_EQ(entry.value().page_epoch, 5u);
    EXPECT_EQ(entry.value().flags & kEntryLive, kEntryLive);
}

TEST_F(WaystoneHooksTest, HeatStartsAtZero) {
    // A bulk load must not look hot: heat is earned by reads, and the
    // insert path has not read anything.
    ASSERT_TRUE(OnInsert(store_, ws_, 1, 700, 0, 0).ok());
    auto entry = LookupEntry(store_, ws_, 1);
    ASSERT_TRUE(entry.ok());
    EXPECT_EQ(entry.value().use_count, 0u);
    EXPECT_EQ(entry.value().last_ts, 0u);
}

TEST_F(WaystoneHooksTest, EveryTupleOfARunOfInsertsIsCovered) {
    // The guarantee in the form it is actually claimed: not "some" and not
    // "the hot ones" - every one.
    constexpr std::uint64_t kCount = 1000;  // spans four entry pages
    for (std::uint64_t pk = 1; pk <= kCount; ++pk) {
        ASSERT_TRUE(OnInsert(store_, ws_, pk, static_cast<PageId>(500 + pk / 100),
                             static_cast<std::uint16_t>(pk % 100), 0)
                        .ok())
            << "pk " << pk;
    }
    for (std::uint64_t pk = 1; pk <= kCount; ++pk) {
        auto entry = LookupEntry(store_, ws_, pk);
        ASSERT_TRUE(entry.ok()) << "pk " << pk << " uncovered: " << entry.status().message();
        EXPECT_EQ(entry.value().pk, pk);
        EXPECT_EQ(entry.value().flags & kEntryLive, kEntryLive);
    }
}

TEST_F(WaystoneHooksTest, TheFirstTupleOfARangeAllocatesItsEntryPageAndTheRestDoNot) {
    const std::size_t before = store_.page_count();

    ASSERT_TRUE(OnInsert(store_, ws_, 0, 700, 0, 0).ok());
    const std::size_t after_first = store_.page_count();
    EXPECT_EQ(after_first, before + 1) << "first touch allocates exactly the leaf";

    // The other 255 pks of that entry page are free.
    for (std::uint64_t pk = 1; pk < kEntriesPerPage; ++pk) {
        ASSERT_TRUE(OnInsert(store_, ws_, pk, 700, 0, 0).ok());
    }
    EXPECT_EQ(store_.page_count(), after_first);

    // Crossing into the next range costs one more.
    ASSERT_TRUE(OnInsert(store_, ws_, kEntriesPerPage, 700, 0, 0).ok());
    EXPECT_EQ(store_.page_count(), after_first + 1);
}

TEST_F(WaystoneHooksTest, ReinsertingThePkOverwritesRatherThanFailing) {
    // Waystone is not a uniqueness index - ids are unique upstream
    // (invariant 10). The hook records the latest observation, full stop.
    ASSERT_TRUE(OnInsert(store_, ws_, 9, 100, 1, 0).ok());
    ASSERT_TRUE(OnInsert(store_, ws_, 9, 200, 2, 7).ok());

    auto entry = LookupEntry(store_, ws_, 9);
    ASSERT_TRUE(entry.ok());
    EXPECT_EQ(entry.value().page_id, 200u);
    EXPECT_EQ(entry.value().slot, 2);
    EXPECT_EQ(entry.value().page_epoch, 7u);
}

// ---- Delete -------------------------------------------------------------

TEST_F(WaystoneHooksTest, DeleteClearsLiveAndKeepsEverythingElse) {
    ASSERT_TRUE(OnInsert(store_, ws_, 77, 800, 4, 11).ok());
    ASSERT_TRUE(OnDelete(store_, ws_, 77).ok());

    auto entry = LookupEntry(store_, ws_, 77);
    ASSERT_TRUE(entry.ok());
    EXPECT_EQ(entry.value().flags & kEntryLive, 0u);
    // A delete-mark is not a physical retirement: the bytes are still
    // there and the entry still says where.
    EXPECT_EQ(entry.value().pk, 77u);
    EXPECT_EQ(entry.value().page_id, 800u);
    EXPECT_EQ(entry.value().slot, 4);
    EXPECT_EQ(entry.value().page_epoch, 11u);
}

TEST_F(WaystoneHooksTest, DeletingAPkWithNoEntryIsANoOpNotAFailure) {
    // Rule 2 of the advisory contract: an incomplete Waystone may cost
    // performance and must never change what a statement does. A DELETE
    // that failed because a backfill had not reached this pk yet would
    // break that outright.
    EXPECT_TRUE(OnDelete(store_, ws_, 12345).ok());
    EXPECT_EQ(store_.page_count(), 1u) << "a delete must never allocate an entry page";
}

TEST_F(WaystoneHooksTest, DeletingTwiceIsHarmless) {
    ASSERT_TRUE(OnInsert(store_, ws_, 5, 100, 0, 0).ok());
    ASSERT_TRUE(OnDelete(store_, ws_, 5).ok());
    ASSERT_TRUE(OnDelete(store_, ws_, 5).ok());

    auto entry = LookupEntry(store_, ws_, 5);
    ASSERT_TRUE(entry.ok());
    EXPECT_EQ(entry.value().flags & kEntryLive, 0u);
}

TEST_F(WaystoneHooksTest, DeletingOneTupleLeavesItsNeighboursLive) {
    for (std::uint64_t pk = 10; pk < 14; ++pk) {
        ASSERT_TRUE(OnInsert(store_, ws_, pk, 100, 0, 0).ok());
    }
    ASSERT_TRUE(OnDelete(store_, ws_, 12).ok());

    for (std::uint64_t pk = 10; pk < 14; ++pk) {
        auto entry = LookupEntry(store_, ws_, pk);
        ASSERT_TRUE(entry.ok());
        const bool live = (entry.value().flags & kEntryLive) != 0;
        EXPECT_EQ(live, pk != 12) << "pk " << pk;
    }
}

// ---- Lookup -------------------------------------------------------------

TEST_F(WaystoneHooksTest, LookingUpAPkNothingEverInsertedIsNotFound) {
    ASSERT_TRUE(OnInsert(store_, ws_, 1, 100, 0, 0).ok());
    // Same entry page as pk 1, so the page exists - the entry in it does
    // not, and the zeroed slot must not read as a live entry for pk 0.
    auto same_page = LookupEntry(store_, ws_, 2);
    ASSERT_TRUE(same_page.ok());
    EXPECT_EQ(same_page.value().flags & kEntryLive, 0u);

    // A pk whose entry page was never allocated at all.
    EXPECT_EQ(LookupEntry(store_, ws_, 99999).status().code(), StatusCode::kNotFound);
}

// ---- Directory growth is the caller's business --------------------------

TEST_F(WaystoneHooksTest, APkPastTheDirectoryIsOutOfRangeNotAnError) {
    // OutOfRange is the "grow and retry" signal, and it must be
    // distinguishable from InvalidArgument, which means the caller is
    // wrong rather than the directory being short.
    const std::uint64_t past = DirCoverageAtDepth(1);
    EXPECT_FALSE(ws_.covers(past));
    EXPECT_EQ(OnInsert(store_, ws_, past, 100, 0, 0).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(OnDelete(store_, ws_, past).code(), StatusCode::kOutOfRange);
}

TEST_F(WaystoneHooksTest, GrowingThenRetryingSucceedsAndKeepsWhatWasThere) {
    // The sequence a caller runs on OutOfRange, minus the catalog write
    // that is not this layer's to make.
    ASSERT_TRUE(OnInsert(store_, ws_, 100, 111, 1, 0).ok());

    const std::uint64_t past = DirCoverageAtDepth(1);
    ASSERT_EQ(OnInsert(store_, ws_, past, 222, 2, 0).code(), StatusCode::kOutOfRange);

    auto grown = GrowDirectory(store_, ws_.dir_root, ws_.depth);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    ws_.dir_root = grown.value();
    ws_.depth += 1;

    ASSERT_TRUE(OnInsert(store_, ws_, past, 222, 2, 0).ok());
    EXPECT_EQ(LookupEntry(store_, ws_, past).value().page_id, 222u);
    // And the tuple inserted before the growth is still covered.
    EXPECT_EQ(LookupEntry(store_, ws_, 100).value().page_id, 111u);
}

TEST_F(WaystoneHooksTest, APkWiderThanTheKeystoneRangeIsInvalidNotGrowable) {
    // No depth would help, so this must not read as "grow and retry".
    EXPECT_EQ(OnInsert(store_, ws_, kMaxPk + 1, 100, 0, 0).code(), StatusCode::kInvalidArgument);
}

TEST_F(WaystoneHooksTest, ARelationWithNoDirectoryIsRefusedByBothHooks) {
    const WaystoneRef none;
    EXPECT_FALSE(none.valid());
    EXPECT_EQ(OnInsert(store_, none, 1, 100, 0, 0).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(OnDelete(store_, none, 1).code(), StatusCode::kInvalidArgument);
}

// ---- Cost ---------------------------------------------------------------

TEST_F(WaystoneHooksTest, AnInsertIntoAnEstablishedRangeTouchesOnlyTheWalkAndTheLeaf) {
    // "Single arithmetic entry write" in the only form a test can see it:
    // once the path exists, an insert allocates nothing at any depth.
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    WaystoneRef deep{root.value(), kMaxDirDepth};

    ASSERT_TRUE(OnInsert(store_, deep, 5000, 100, 0, 0).ok());
    const std::size_t settled = store_.page_count();

    for (std::uint64_t pk = 5001; pk < 5100; ++pk) {
        ASSERT_TRUE(OnInsert(store_, deep, pk, 100, 0, 0).ok());
    }
    EXPECT_EQ(store_.page_count(), settled);
}

}  // namespace
}  // namespace kds::stats
