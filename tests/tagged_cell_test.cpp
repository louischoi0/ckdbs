#include "kds/storage/tagged_cell.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// The tagged cell (docs/rules/rule-fixed-length-tuple.md sections 3 and 8.1): the
// fixed-width slot every variable-width value occupies.
//
// Two things are being defended here. First, the boundary - the spill
// decision is a pure function of value length, so the exact byte at which a
// value stops fitting is a format property and not an implementation
// detail. Second, and less obviously, the **zero padding**: a cell is
// overwritten in place by an UPDATE, so a shorter new value written over a
// longer old one must not leave the old tail readable underneath it. That
// is not a cosmetic concern - those bytes reach a page image, an undo
// record and a checksum.

namespace kds::storage {
namespace {

constexpr std::uint32_t kW = kDefaultInlineCellWidth;
constexpr std::uint32_t kCapacity = InlineCapacity(kW);

using Cell = std::vector<std::byte>;

Cell FreshCell(std::uint32_t width = kW) { return Cell(width, std::byte{0}); }

std::string TextOf(const CellValue& value) {
    std::string out;
    for (std::byte b : value.bytes) out.push_back(static_cast<char>(b));
    return out;
}

// ---- Round trips ---------------------------------------------------------

TEST(TaggedCellTest, InlineValueRoundTrips) {
    Cell cell = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(cell, "alice").ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().tag, CellTag::kInline);
    EXPECT_EQ(TextOf(decoded.value()), "alice");
}

TEST(TaggedCellTest, TheEmptyStringIsInlineNotNull) {
    // The whole reason the format carries a tag byte rather than a
    // sentinel length: an empty value and a NULL are different facts, and
    // neither may be inferred from a length of zero.
    Cell cell = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(cell, "").ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().tag, CellTag::kInline);
    EXPECT_TRUE(decoded.value().bytes.empty());
}

TEST(TaggedCellTest, NullRoundTrips) {
    Cell cell = FreshCell();
    ASSERT_TRUE(EncodeNullCell(cell).ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().tag, CellTag::kNull);
    EXPECT_TRUE(decoded.value().bytes.empty());
}

TEST(TaggedCellTest, ValueBytesAreNotInterpreted) {
    // A cell holds opaque bytes: an embedded NUL is data, not a
    // terminator. `char` columns are the ones that stop at a NUL, and this
    // is not one.
    Cell cell = FreshCell();
    const std::string embedded("a\0b", 3);
    ASSERT_TRUE(EncodeInlineCell(cell, embedded).ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().bytes.size(), 3u);
    EXPECT_EQ(TextOf(decoded.value()), embedded);
}

// ---- The boundary --------------------------------------------------------

TEST(TaggedCellTest, TheLastValueThatFitsInlineIsAccepted) {
    Cell cell = FreshCell();
    const std::string exact(kCapacity, 'x');
    ASSERT_TRUE(EncodeInlineCell(cell, exact).ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().bytes.size(), kCapacity);
    EXPECT_EQ(TextOf(decoded.value()), exact);
}

TEST(TaggedCellTest, OneByteOverTheCapacityIsRefusedInline) {
    Cell cell = FreshCell();
    Status s = EncodeInlineCell(cell, std::string(kCapacity + 1, 'x'));

    ASSERT_FALSE(s.ok());
    // OutOfSpace, and that code is the contract rather than a detail: it is
    // the signal the row codec branches on to spill instead. An error class
    // here would make "too long to inline" indistinguishable from "wrong".
    EXPECT_EQ(s.code(), StatusCode::kOutOfSpace);
    EXPECT_NE(s.message().find("var-heap"), std::string::npos) << s.message();
}

TEST(TaggedCellTest, CapacityIsTheWidthLessTheTagAndLength) {
    // Stated as arithmetic rather than as a number, so a change to the
    // header layout has to change this too.
    EXPECT_EQ(kCapacity, kW - 3);
    EXPECT_EQ(InlineCapacity(kMinInlineCellWidth), kMinInlineCellWidth - 3);
}

// ---- Padding: no stale bytes ---------------------------------------------

TEST(TaggedCellTest, AShortValueOverAlongOneLeavesNoStaleBytes) {
    // The property that matters on the UPDATE path. Written as a byte
    // comparison against a never-used cell rather than by decoding,
    // because decoding is exactly what would *not* notice the leak.
    Cell reused = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(reused, std::string(kCapacity, 'A')).ok());
    ASSERT_TRUE(EncodeInlineCell(reused, "ab").ok());

    Cell pristine = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(pristine, "ab").ok());

    EXPECT_EQ(reused, pristine);
}

TEST(TaggedCellTest, NullOverAValueLeavesNoStaleBytes) {
    Cell reused = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(reused, std::string(kCapacity, 'A')).ok());
    ASSERT_TRUE(EncodeNullCell(reused).ok());

    EXPECT_EQ(reused, FreshCell());  // a kNull cell is the tag (0) then zeros
}

// ---- Refusals ------------------------------------------------------------

TEST(TaggedCellTest, ACellNarrowerThanTheLegalMinimumIsRefused) {
    Cell tiny = FreshCell(kMinInlineCellWidth - 1);
    EXPECT_EQ(EncodeInlineCell(tiny, "x").code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(EncodeNullCell(tiny).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(DecodeCell(tiny).status().code(), StatusCode::kInvalidArgument);
}

TEST(TaggedCellTest, AnUnassignedTagIsCorruption) {
    Cell cell = FreshCell();
    cell[kCellTagOffset] = static_cast<std::byte>(kMaxAssignedCellTag + 1);

    auto decoded = DecodeCell(cell);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

TEST(TaggedCellTest, AnInlineLengthPastTheCapacityIsCorruptionNotAReadPastTheCell) {
    // The failure this prevents is the interesting one: sizing a read from
    // a length that the page and the schema disagree about.
    Cell cell = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(cell, "abc").ok());
    const std::uint16_t lying = static_cast<std::uint16_t>(kCapacity + 1);
    cell[kCellInlineLenOffset] = static_cast<std::byte>(lying & 0xFF);
    cell[kCellInlineLenOffset + 1] = static_cast<std::byte>((lying >> 8) & 0xFF);

    auto decoded = DecodeCell(cell);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

TEST(TaggedCellTest, SpilledCellRoundTripsItsLengthAndPointer) {
    // A spilled cell carries a pointer, and decoding it does *not* resolve
    // that pointer - this layer has no page store and wants none. The
    // length rides along so a reader knows the value's size without a fetch.
    Cell cell = FreshCell();
    constexpr std::uint32_t kLen = 5000;
    constexpr std::uint64_t kPtr = 0x0000'02BC'0007'0000ULL;
    ASSERT_TRUE(EncodeSpilledCell(cell, kLen, kPtr).ok());

    auto decoded = DecodeCell(cell);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().tag, CellTag::kSpilled);
    EXPECT_EQ(decoded.value().len, kLen);
    EXPECT_EQ(decoded.value().varheap_ptr, kPtr);
    EXPECT_TRUE(decoded.value().bytes.empty());
}

TEST(TaggedCellTest, SpillingOverALongInlineValueLeavesNoStaleBytes) {
    // The tag flip an UPDATE performs when a value crosses the boundary.
    // The cell is the same size either way - that is invariant 13 - so what
    // has to be checked is that the old text is gone, not that the cell
    // moved.
    Cell reused = FreshCell();
    ASSERT_TRUE(EncodeInlineCell(reused, std::string(kCapacity, 'A')).ok());
    ASSERT_TRUE(EncodeSpilledCell(reused, 9000, 0x1234'5678'0001'0000ULL).ok());

    Cell pristine = FreshCell();
    ASSERT_TRUE(EncodeSpilledCell(pristine, 9000, 0x1234'5678'0001'0000ULL).ok());
    EXPECT_EQ(reused, pristine);
}

// ---- The width constant --------------------------------------------------

TEST(TaggedCellTest, WidthBoundsAreCheckedAtTheEdges) {
    EXPECT_TRUE(CheckInlineCellWidth(kMinInlineCellWidth).ok());
    EXPECT_TRUE(CheckInlineCellWidth(kMaxInlineCellWidth).ok());
    EXPECT_TRUE(CheckInlineCellWidth(kDefaultInlineCellWidth).ok());

    EXPECT_FALSE(CheckInlineCellWidth(kMinInlineCellWidth - 1).ok());
    EXPECT_FALSE(CheckInlineCellWidth(kMaxInlineCellWidth + 1).ok());
    EXPECT_FALSE(CheckInlineCellWidth(0).ok());
}

TEST(TaggedCellTest, TheNarrowestLegalWidthStillHoldsASpilledDescriptor) {
    // Why kMinInlineCellWidth is what it is: a width that could not hold a
    // spilled descriptor would make spilling unrepresentable, which is a
    // format hole rather than a missing feature.
    EXPECT_GE(kMinInlineCellWidth, kCellSpilledSize);
    EXPECT_EQ(kCellSpilledSize, kCellSpilledPtrOffset + 8);
}

// ---- Any legal width, not just the default -------------------------------

TEST(TaggedCellTest, EveryLegalWidthRoundTripsAtItsOwnBoundary) {
    // The default width is [PROPOSED] and expected to move once measured,
    // so nothing here may depend on it being 64.
    for (std::uint32_t width : {kMinInlineCellWidth, 32u, kDefaultInlineCellWidth, 256u,
                                kMaxInlineCellWidth}) {
        Cell cell = FreshCell(width);
        const std::string exact(InlineCapacity(width), 'z');

        ASSERT_TRUE(EncodeInlineCell(cell, exact).ok()) << "width " << width;
        auto decoded = DecodeCell(cell);
        ASSERT_TRUE(decoded.ok()) << "width " << width;
        EXPECT_EQ(TextOf(decoded.value()), exact) << "width " << width;

        EXPECT_FALSE(EncodeInlineCell(cell, exact + "z").ok()) << "width " << width;
    }
}

}  // namespace
}  // namespace kds::storage
