#include "kds/stats/waystone.hpp"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "kds/storage/page_header.hpp"

// Codec tests for the waystone page (docs/spec/waystone-concpets.md §6). No
// PageStore and no directory: this is encode/decode over one page buffer,
// which is the level the offsets, the bounds and the identity check live
// at.

namespace kds::stats {
namespace {

class WaystonePageTest : public ::testing::Test {
protected:
    std::span<std::byte, kPageSize> page() {
        return std::span<std::byte, kPageSize>(buf_.data(), kPageSize);
    }
    std::span<const std::byte, kPageSize> const_page() const {
        return std::span<const std::byte, kPageSize>(buf_.data(), kPageSize);
    }

    std::array<std::byte, kPageSize> buf_{};
};

WaystoneEntry SampleEntry() {
    WaystoneEntry e{};
    e.pk = 0x000000FFEEDDCCBBull;  // inside the 40-bit id space
    e.rel_oid = 0x1122334455667788ull;
    e.page_id = 0xAABBCCDDu;
    e.page_epoch = 0x01020304u;
    e.slot = 0xBEEF;
    e.flags = kWaystoneEntryValid;
    e.step_id = 0x0102;
    e.reserved = 0;
    return e;
}

// ---- Derived sizes --------------------------------------------------------

TEST(WaystoneLayoutTest, EntriesPerPageFollowFromTheHeaderSizes) {
    // 8192 - 32 (common header) - 40 (waystone header) = 8120; / 32 = 253.
    EXPECT_EQ(kEntriesPerWaystonePage, 253u);
    EXPECT_EQ(kWaystoneBodyOffset, storage::kPageBodyOffset + kWaystoneHeaderSize);

    // Deliberately not a power of two, and deliberately leaving slack:
    // nothing addresses an entry by shift and mask, which is what buys the
    // page its common header back.
    const std::size_t used = kWaystoneBodyOffset + kEntriesPerWaystonePage * kWaystoneEntrySize;
    EXPECT_LE(used, kPageSize);
    EXPECT_EQ(kPageSize - used, 24u);
}

// ---- Format ---------------------------------------------------------------

TEST_F(WaystonePageTest, FormatWritesAHeaderedWaystonePage) {
    FormatWaystonePage(page(), {0xAAAA, 0xBBBB}, 1234);

    // It is a real headered page: the store will checksum it and the WAL
    // can stamp it, neither of which was possible for the headerless
    // structure this replaces.
    ASSERT_TRUE(storage::ValidatePageHeader(const_page(), PageType::kWaystone).ok());
    const storage::PageHeaderFields common = storage::ReadPageHeader(const_page());
    EXPECT_EQ(common.page_type, static_cast<std::uint8_t>(PageType::kWaystone));
    EXPECT_EQ(common.page_lsn, storage::kNoPageLsn);

    const WaystoneHeader h = ReadWaystoneHeader(const_page());
    EXPECT_EQ(h.pattern_id, 0xAAAAu);
    EXPECT_EQ(h.arg_hash, 0xBBBBu);
    EXPECT_EQ(h.recorded_ts, 1234u);
    EXPECT_EQ(h.entry_count, 0u);
    EXPECT_EQ(h.use_count, 0u);
    EXPECT_EQ(h.next_page_id, kInvalidPageId);
}

TEST_F(WaystonePageTest, AFreshPageHasNoValidEntries) {
    FormatWaystonePage(page(), {1, 2}, 0);

    // A never-written entry decodes to pk 0 and page_id 0, both of which
    // look like real values. Only the flag says whether it means anything.
    auto e = ReadWaystoneEntry(const_page(), 0);
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e.value().flags & kWaystoneEntryValid, 0u);
    EXPECT_EQ(e.value().pk, 0u);
}

// ---- Header codec ---------------------------------------------------------

TEST_F(WaystonePageTest, HeaderRoundTripsEveryField) {
    FormatWaystonePage(page(), {0, 0}, 0);

    WaystoneHeader in{};
    in.pattern_id = 0x1122334455667788ull;
    in.arg_hash = 0x99AABBCCDDEEFF00ull;
    in.recorded_ts = 0x0102030405060708ull;
    in.next_page_id = 4096;
    in.use_count = 0xC0FFEEu;
    in.entry_count = 7;
    in.flags = 0x5A5A;
    in.reserved = 0;
    ASSERT_TRUE(WriteWaystoneHeader(page(), in).ok());

    const WaystoneHeader out = ReadWaystoneHeader(const_page());
    EXPECT_EQ(out.pattern_id, in.pattern_id);
    EXPECT_EQ(out.arg_hash, in.arg_hash);
    EXPECT_EQ(out.recorded_ts, in.recorded_ts);
    EXPECT_EQ(out.next_page_id, in.next_page_id);
    EXPECT_EQ(out.use_count, in.use_count);
    EXPECT_EQ(out.entry_count, in.entry_count);
    EXPECT_EQ(out.flags, in.flags);
}

TEST_F(WaystonePageTest, HeaderWriteDoesNotDisturbTheCommonHeader) {
    FormatWaystonePage(page(), {1, 2}, 3);

    WaystoneHeader h = ReadWaystoneHeader(const_page());
    h.entry_count = 3;
    ASSERT_TRUE(WriteWaystoneHeader(page(), h).ok());

    // The waystone header sits *after* the common one; writing it must not
    // reach back over the page type or the page_lsn.
    EXPECT_TRUE(storage::ValidatePageHeader(const_page(), PageType::kWaystone).ok());
}

TEST_F(WaystonePageTest, AnEntryCountThePageCannotHoldIsRefused) {
    FormatWaystonePage(page(), {1, 2}, 3);

    WaystoneHeader h = ReadWaystoneHeader(const_page());
    h.entry_count = static_cast<std::uint16_t>(kEntriesPerWaystonePage + 1);
    auto s = WriteWaystoneHeader(page(), h);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);

    // The boundary itself is fine - a full page is a legal page.
    h.entry_count = static_cast<std::uint16_t>(kEntriesPerWaystonePage);
    EXPECT_TRUE(WriteWaystoneHeader(page(), h).ok());
}

// ---- Entry codec ----------------------------------------------------------

TEST_F(WaystonePageTest, EntryRoundTripsEveryField) {
    FormatWaystonePage(page(), {1, 2}, 3);
    const WaystoneEntry in = SampleEntry();
    ASSERT_TRUE(WriteWaystoneEntry(page(), 5, in).ok());

    auto out = ReadWaystoneEntry(const_page(), 5);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().pk, in.pk);
    EXPECT_EQ(out.value().rel_oid, in.rel_oid);
    EXPECT_EQ(out.value().page_id, in.page_id);
    EXPECT_EQ(out.value().page_epoch, in.page_epoch);
    EXPECT_EQ(out.value().slot, in.slot);
    EXPECT_EQ(out.value().flags, in.flags);
    EXPECT_EQ(out.value().step_id, in.step_id);
}

TEST_F(WaystonePageTest, AFullWidthRelOidSurvives) {
    // The field is a catalog::Oid, which is 64-bit. A 32-bit field here
    // would truncate two distinct relations onto one value, which is an
    // aliasing bug rather than a lost byte.
    FormatWaystonePage(page(), {1, 2}, 3);
    WaystoneEntry in = SampleEntry();
    in.rel_oid = 0xFFFFFFFFFFFFFFFFull;
    ASSERT_TRUE(WriteWaystoneEntry(page(), 0, in).ok());

    auto out = ReadWaystoneEntry(const_page(), 0);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().rel_oid, 0xFFFFFFFFFFFFFFFFull);
}

TEST_F(WaystonePageTest, AdjacentEntriesDoNotOverlap) {
    // The bug a single round-trip cannot catch: an off-by-one in the entry
    // stride, which only shows up when two entries are live at once.
    FormatWaystonePage(page(), {1, 2}, 3);

    for (std::size_t i = 0; i < kEntriesPerWaystonePage; ++i) {
        WaystoneEntry e{};
        e.pk = i + 1;
        e.rel_oid = 1000 + i;
        e.step_id = static_cast<std::uint16_t>(i);
        e.flags = kWaystoneEntryValid;
        ASSERT_TRUE(WriteWaystoneEntry(page(), i, e).ok()) << i;
    }
    for (std::size_t i = 0; i < kEntriesPerWaystonePage; ++i) {
        auto e = ReadWaystoneEntry(const_page(), i);
        ASSERT_TRUE(e.ok()) << i;
        EXPECT_EQ(e.value().pk, i + 1) << i;
        EXPECT_EQ(e.value().rel_oid, 1000u + i) << i;
        EXPECT_EQ(e.value().step_id, i) << i;
    }
}

TEST_F(WaystonePageTest, TheLastEntryStaysInsideThePage) {
    FormatWaystonePage(page(), {1, 2}, 3);

    // Writing the highest legal index must not touch the 24 bytes of tail
    // slack, and one past it must be refused rather than run off the end.
    WaystoneEntry e = SampleEntry();
    ASSERT_TRUE(WriteWaystoneEntry(page(), kEntriesPerWaystonePage - 1, e).ok());

    auto too_far = WriteWaystoneEntry(page(), kEntriesPerWaystonePage, e);
    EXPECT_FALSE(too_far.ok());
    EXPECT_EQ(too_far.code(), StatusCode::kOutOfRange);
    EXPECT_EQ(ReadWaystoneEntry(const_page(), kEntriesPerWaystonePage).status().code(),
              StatusCode::kOutOfRange);

    // Tail slack untouched.
    const std::size_t used = kWaystoneBodyOffset + kEntriesPerWaystonePage * kWaystoneEntrySize;
    for (std::size_t i = used; i < kPageSize; ++i) {
        EXPECT_EQ(std::to_integer<int>(buf_[i]), 0) << i;
    }
}

TEST_F(WaystonePageTest, WritingAnEntryDoesNotDisturbTheHeader) {
    FormatWaystonePage(page(), {0xAAAA, 0xBBBB}, 77);
    ASSERT_TRUE(WriteWaystoneEntry(page(), 0, SampleEntry()).ok());

    const WaystoneHeader h = ReadWaystoneHeader(const_page());
    EXPECT_EQ(h.pattern_id, 0xAAAAu);
    EXPECT_EQ(h.arg_hash, 0xBBBBu);
    EXPECT_EQ(h.recorded_ts, 77u);
}

// ---- Identity: the check that makes a hash collision survivable ----------

TEST_F(WaystonePageTest, APageHoldsOnlyTheInstanceItRecorded) {
    FormatWaystonePage(page(), {0xAAAA, 0xBBBB}, 0);

    EXPECT_TRUE(WaystonePageHolds(const_page(), {0xAAAA, 0xBBBB}));

    // The directory is keyed by a hash, so a collision leads a reader to a
    // real, valid, *wrong* trail. This is what turns that into a miss
    // instead of somebody else's rows.
    EXPECT_FALSE(WaystonePageHolds(const_page(), {0xAAAA, 0xCCCC}));
    EXPECT_FALSE(WaystonePageHolds(const_page(), {0xDDDD, 0xBBBB}));
    EXPECT_FALSE(WaystonePageHolds(const_page(), {0xDDDD, 0xCCCC}));
}

TEST_F(WaystonePageTest, AnUnformattedPageHoldsNothing) {
    // All-zero, which is what a sparse never-written page reads back as.
    EXPECT_FALSE(WaystonePageHolds(const_page(), {0, 0}));
}

TEST_F(WaystonePageTest, APageOfAnotherTypeHoldsNothing) {
    // A heap page whose body bytes happen to look like a matching header.
    storage::FormatPage(page(), PageType::kHeap);
    WaystoneHeader h{};
    h.pattern_id = 0xAAAA;
    h.arg_hash = 0xBBBB;
    ASSERT_TRUE(WriteWaystoneHeader(page(), h).ok());

    EXPECT_FALSE(WaystonePageHolds(const_page(), {0xAAAA, 0xBBBB}));
}

TEST_F(WaystonePageTest, APageFromANewerBuildHoldsNothing) {
    FormatWaystonePage(page(), {0xAAAA, 0xBBBB}, 0);

    storage::PageHeaderFields common = storage::ReadPageHeader(const_page());
    common.format_version = static_cast<std::uint8_t>(common.format_version + 1);
    storage::WritePageHeader(page(), common);

    // Refused rather than misparsed: a layout this build does not know is
    // not a trail it can replay.
    EXPECT_FALSE(WaystonePageHolds(const_page(), {0xAAAA, 0xBBBB}));
}

}  // namespace
}  // namespace kds::stats
