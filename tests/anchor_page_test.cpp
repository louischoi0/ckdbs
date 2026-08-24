#include "kds/storage/anchor_page.hpp"

#include <array>

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

    EXPECT_EQ(AnchorIndexRoot(Const(page), 9001), kInvalidPageId);
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9001, 300).ok());
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9002, 301).ok());
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9001), 300u);
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9002), 301u);

    // Update in place: a root move rewrites the entry, never appends.
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9001, 310).ok());
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9001), 310u);
}

TEST(AnchorPageTest, RemoveSwapsWithLastAndTwiceIsANoOp) {
    Page page{};
    FormatAnchorPage(Mut(page), 4001, 130);
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9001, 300).ok());
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9002, 301).ok());
    ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 9003, 302).ok());

    RemoveAnchorIndexRoot(Mut(page), 9001);
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9001), kInvalidPageId);
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9002), 301u) << "survivors keep their roots";
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9003), 302u);

    // DROP INDEX's compensation may run twice; absence is a no-op.
    RemoveAnchorIndexRoot(Mut(page), 9001);
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9002), 301u);
    EXPECT_EQ(AnchorIndexRoot(Const(page), 9003), 302u);
}

TEST(AnchorPageTest, TheEntryTableRefusesPastCapacity) {
    Page page{};
    FormatAnchorPage(Mut(page), 4001, 130);
    for (std::size_t i = 0; i < kAnchorMaxIndexEntries; ++i) {
        ASSERT_TRUE(SetAnchorIndexRoot(Mut(page), 10000 + i,
                                       static_cast<PageId>(500 + i))
                        .ok())
            << i;
    }
    Status refused = SetAnchorIndexRoot(Mut(page), 99999, 900);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kResourceExhausted);
    // An existing entry still updates at capacity - fullness refuses
    // growth, never a root move.
    EXPECT_TRUE(SetAnchorIndexRoot(Mut(page), 10000, 999).ok());
    EXPECT_EQ(AnchorIndexRoot(Const(page), 10000), 999u);
}

}  // namespace
}  // namespace kds::storage
