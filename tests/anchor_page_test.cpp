#include "kds/storage/anchor_page.hpp"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

// PW2-1 (workplan-peer-writer.md §7a): the relation anchor page, the one
// fixed page whose mutations replace every growth-path catalog write.

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> Mut(Page& p) { return std::span<std::byte, kPageSize>(p); }
std::span<const std::byte, kPageSize> Const(const Page& p) {
    return std::span<const std::byte, kPageSize>(p);
}

PageId Root(const Page& p, std::uint64_t index_oid) {
    auto r = AnchorIndexRoot(Const(p), index_oid);
    EXPECT_TRUE(r.ok()) << r.status().message();
    return r.ok() ? r.value() : 0xDEADu;
}

TEST(AnchorPageTest, FormatCarriesTypeOwnerAndClusteredRoot) {
    Page page{};
    FormatAnchorPage(Mut(page), /*owner_oid=*/4001, /*clustered_root=*/130);

    EXPECT_EQ(RawPageType(Const(page)), static_cast<std::uint8_t>(PageType::kAnchor));
    EXPECT_EQ(GetOwnerOid(Const(page)), 4001u);
    EXPECT_EQ(AnchorClusteredRoot(Const(page)), 130u);
    EXPECT_TRUE(ValidatePageHeader(Const(page), PageType::kAnchor).ok())
        << "a fresh anchor must pass its own validator - the kCabinBound "
           "format-version bug's regression direction";

    SetAnchorClusteredRoot(Mut(page), 262);
    EXPECT_EQ(AnchorClusteredRoot(Const(page)), 262u);
}

TEST(AnchorPageTest, IndexRootsInsertUpdateAndLookUp) {
    Page page{};
    FormatAnchorPage(Mut(page), 4001, 130);

    EXPECT_EQ(Root(page, 9001), kInvalidPageId);
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9001, 300).ok());
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9002, 301).ok());
    EXPECT_EQ(Root(page, 9001), 300u);
    EXPECT_EQ(Root(page, 9002), 301u);

    // Update in place: a root move rewrites the entry, never appends.
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9001, 310).ok());
    EXPECT_EQ(Root(page, 9001), 310u);
}

TEST(AnchorPageTest, AForgedEntryCountIsCorruptionNeverALoopBound) {
    // The 3f07eda review's C1, pinned: nr_index duplicates a schema
    // constant, so it is checked redundancy (docs/rules/rules.md) - a count the
    // page cannot hold refuses, in both the read and the write direction.
    // Unchecked, it was an ASan-demonstrated out-of-bounds read with an
    // out-of-bounds write one branch over.
    Page page{};
    FormatAnchorPage(Mut(page), 4001, 130);
    const std::uint16_t forged = 4000;
    std::memcpy(page.data() + kAnchorNrIndexOffset, &forged, sizeof(forged));

    auto read = AnchorIndexRoot(Const(page), 9001);
    ASSERT_FALSE(read.ok());
    EXPECT_EQ(read.status().code(), StatusCode::kCorruption) << read.status().message();

    Status write = SetAnchorIndexRoot(Mut(page), 9001, 300);
    ASSERT_FALSE(write.ok());
    EXPECT_EQ(write.code(), StatusCode::kCorruption) << write.message();

    // And in the pre-check, which reads the same count through the same
    // helper: a forged bound must not become a bound to loop on there
    // either.
    EXPECT_EQ(CheckAnchorRoomForIndex(Const(page), 9001).code(), StatusCode::kCorruption);
}

TEST(AnchorPageTest, TheEntryTableRefusesPastCapacityAndTheCheckSaysSoFirst) {
    // The cap, and G2's pre-check beside it in one fixture, because the
    // check is only worth having if it answers *exactly* what the write
    // would: `CREATE INDEX` builds the tree and seeds the slot afterwards,
    // so a refusal raised at the seed costs a whole index tree, and
    // nothing frees.
    Page page{};
    FormatAnchorPage(Mut(page), 4001, 130);
    EXPECT_TRUE(CheckAnchorRoomForIndex(Const(page), 7001).ok());

    for (std::size_t i = 0; i < kAnchorMaxIndexEntries; ++i) {
        // The last entry is the positive boundary: with one slot left both
        // must still admit, or the check refuses an index that fits.
        if (i + 1 == kAnchorMaxIndexEntries) {
            ASSERT_TRUE(CheckAnchorRoomForIndex(Const(page), 10000 + i).ok()) << i;
        }
        ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 10000 + i, static_cast<PageId>(500 + i)).ok())
            << i;
    }

    Status checked = CheckAnchorRoomForIndex(Const(page), 99999);
    Status refused = SetAnchorIndexRoot(Mut(page), 99999, 900);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kResourceExhausted);
    EXPECT_EQ(checked.code(), refused.code());
    EXPECT_EQ(checked.message(), refused.message())
        << "one refusal, or the check and the write can drift apart";

    // An existing entry still updates at capacity - fullness refuses
    // growth, never a root move - and the check must not refuse one.
    EXPECT_TRUE(CheckAnchorRoomForIndex(Const(page), 10000).ok());
    EXPECT_TRUE(SetAnchorIndexRoot(Mut(page), 10000, 999).ok());
    EXPECT_EQ(Root(page, 10000), 999u);
}

}  // namespace
}  // namespace kds::storage
