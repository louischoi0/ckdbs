#include "kds/stats/waystone.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

// The entry codec (waystone-concpets.md section 5, spec test 12-1). What
// matters here is that the format is *pinned*: these entries are written
// to pages that outlive the process, so a field that quietly moves is a
// database that quietly reads garbage.

namespace kds::stats {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> Mut(Page& page) {
    return std::span<std::byte, kPageSize>(page);
}
std::span<const std::byte, kPageSize> Const(const Page& page) {
    return std::span<const std::byte, kPageSize>(page);
}

WaystoneEntry Sample(std::uint64_t pk) {
    WaystoneEntry entry;
    entry.pk = pk;
    entry.page_id = 4242;
    entry.slot = 77;
    entry.flags = kEntryLive;
    entry.use_count = 0xDEADBEEF;
    entry.last_ts = 0x01020304;
    entry.page_epoch = 9;
    entry.reserved = 0;
    return entry;
}

void ExpectSame(const WaystoneEntry& a, const WaystoneEntry& b) {
    EXPECT_EQ(a.pk, b.pk);
    EXPECT_EQ(a.page_id, b.page_id);
    EXPECT_EQ(a.slot, b.slot);
    EXPECT_EQ(a.flags, b.flags);
    EXPECT_EQ(a.use_count, b.use_count);
    EXPECT_EQ(a.last_ts, b.last_ts);
    EXPECT_EQ(a.page_epoch, b.page_epoch);
    EXPECT_EQ(a.reserved, b.reserved);
}

TEST(WaystoneEntryTest, EveryFieldRoundTrips) {
    Page page{};
    const WaystoneEntry written = Sample(0x0000'00FF'FFFF'FFFFull);  // widest legal pk
    ASSERT_TRUE(WriteEntry(Mut(page), 0, written).ok());

    auto read = ReadEntry(Const(page), 0);
    ASSERT_TRUE(read.ok()) << read.status().message();
    ExpectSame(written, read.value());
}

TEST(WaystoneEntryTest, EntriesTileThePageExactlyAndDoNotOverlap) {
    Page page{};
    // Every slot gets a distinguishable entry; if any write bled into a
    // neighbour, some read below would come back with the wrong pk.
    for (std::size_t i = 0; i < kEntriesPerPage; ++i) {
        WaystoneEntry entry = Sample(1000 + i);
        entry.slot = static_cast<std::uint16_t>(i);
        entry.page_id = static_cast<PageId>(500 + i);
        ASSERT_TRUE(WriteEntry(Mut(page), i, entry).ok());
    }
    for (std::size_t i = 0; i < kEntriesPerPage; ++i) {
        auto read = ReadEntry(Const(page), i);
        ASSERT_TRUE(read.ok()) << read.status().message();
        EXPECT_EQ(read.value().pk, 1000 + i);
        EXPECT_EQ(read.value().slot, i);
        EXPECT_EQ(read.value().page_id, 500 + i);
    }
}

TEST(WaystoneEntryTest, TheLastEntryEndsExactlyAtThePageEnd) {
    // 256 * 32 = 8192 with nothing left over. If the tiling were off by an
    // entry this would either fail the bound or write past the page.
    static_assert(kEntriesPerPage * kEntrySize == kPageSize);

    Page page{};
    ASSERT_TRUE(WriteEntry(Mut(page), kEntriesPerPage - 1, Sample(7)).ok());
    auto read = ReadEntry(Const(page), kEntriesPerPage - 1);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().pk, 7u);
}

TEST(WaystoneEntryTest, ASlotPastTheEndIsOutOfRangeRatherThanAWildWrite) {
    Page page{};
    EXPECT_EQ(WriteEntry(Mut(page), kEntriesPerPage, Sample(1)).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(ReadEntry(Const(page), kEntriesPerPage).status().code(), StatusCode::kOutOfRange);
}

TEST(WaystoneEntryTest, APkWiderThanFortyBitsIsRefused) {
    Page page{};
    WaystoneEntry entry = Sample(kMaxPk + 1);
    EXPECT_EQ(WriteEntry(Mut(page), 0, entry).code(), StatusCode::kInvalidArgument);
}

TEST(WaystoneEntryTest, AZeroedEntryIsNotLive) {
    // The distinction the coverage guarantee rests on: a never-written
    // entry and a written one for pk 0 are the same bytes except for the
    // flag, so callers must test kEntryLive and not pk != 0.
    const Page page{};
    auto read = ReadEntry(Const(page), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().pk, 0u);
    EXPECT_EQ(read.value().flags & kEntryLive, 0u);
}

TEST(WaystoneEntryTest, ClearingLiveLeavesTheRestOfTheEntryIntact) {
    // What OnDelete does (spec section 9): heat and location survive a
    // delete-mark, only liveness changes.
    Page page{};
    const WaystoneEntry live = Sample(55);
    ASSERT_TRUE(WriteEntry(Mut(page), 3, live).ok());

    auto read = ReadEntry(Const(page), 3);
    ASSERT_TRUE(read.ok());
    WaystoneEntry dead = read.value();
    dead.flags = static_cast<std::uint16_t>(dead.flags & ~kEntryLive);
    ASSERT_TRUE(WriteEntry(Mut(page), 3, dead).ok());

    auto after = ReadEntry(Const(page), 3);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().flags & kEntryLive, 0u);
    EXPECT_EQ(after.value().pk, live.pk);
    EXPECT_EQ(after.value().page_id, live.page_id);
    EXPECT_EQ(after.value().use_count, live.use_count);
}

TEST(WaystoneEntryTest, WritingOneEntryLeavesItsNeighboursUntouched) {
    Page page{};
    ASSERT_TRUE(WriteEntry(Mut(page), 10, Sample(100)).ok());
    ASSERT_TRUE(WriteEntry(Mut(page), 11, Sample(200)).ok());
    ASSERT_TRUE(WriteEntry(Mut(page), 12, Sample(300)).ok());

    WaystoneEntry overwrite = Sample(999);
    overwrite.use_count = 1;
    ASSERT_TRUE(WriteEntry(Mut(page), 11, overwrite).ok());

    EXPECT_EQ(ReadEntry(Const(page), 10).value().pk, 100u);
    EXPECT_EQ(ReadEntry(Const(page), 11).value().pk, 999u);
    EXPECT_EQ(ReadEntry(Const(page), 12).value().pk, 300u);
}

// ---- Addressing (spec section 4) ----------------------------------------

TEST(WaystoneAddressingTest, PkSplitsIntoPageIndexAndSlot) {
    EXPECT_EQ(LogicalEntryPageOf(0), 0u);
    EXPECT_EQ(EntrySlotOf(0), 0u);

    EXPECT_EQ(LogicalEntryPageOf(255), 0u);
    EXPECT_EQ(EntrySlotOf(255), 255u);

    // The first pk of the second entry page.
    EXPECT_EQ(LogicalEntryPageOf(256), 1u);
    EXPECT_EQ(EntrySlotOf(256), 0u);

    EXPECT_EQ(LogicalEntryPageOf(kMaxPk), kMaxPk >> 8);
    EXPECT_EQ(EntrySlotOf(kMaxPk), 255u);
}

TEST(WaystoneAddressingTest, EveryPkInAPageMapsToADistinctSlot) {
    const std::uint64_t base = 4096;  // page-aligned
    for (std::size_t i = 0; i < kEntriesPerPage; ++i) {
        EXPECT_EQ(LogicalEntryPageOf(base + i), base >> 8);
        EXPECT_EQ(EntrySlotOf(base + i), i);
    }
    // One past the page rolls over, and does so exactly once.
    EXPECT_EQ(LogicalEntryPageOf(base + kEntriesPerPage), (base >> 8) + 1);
    EXPECT_EQ(EntrySlotOf(base + kEntriesPerPage), 0u);
}

}  // namespace
}  // namespace kds::stats
