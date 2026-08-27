#include "kds/catalog/rows.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/core_placement.hpp"
#include "kds/parser/fingerprint.hpp"

// Pure codec tests for sys.patterns rows (docs/spec/waystone-concpets.md
// section 4). No Catalog, no PageStore: this is Encode/Decode over a byte
// span and nothing else, which is the level the offsets and the exact-size
// rule actually live at.
//
// The other catalog rows predate this file and are covered indirectly
// through tests/catalog_test.cpp. That is thinner than it looks - an
// offset collision between two fields survives any test that only ever
// writes one row and reads it back with the same code - which is why the
// field-independence and byte-layout tests below exist here.

namespace kds::catalog {
namespace {

SysPatternRow SampleRow() {
    SysPatternRow row{};
    row.oid = 0x0102030405060708ull;
    row.pattern_id = 0x1122334455667788ull;
    row.last_seen = 0x99aabbccddeeff00ull;
    row.fingerprint_version = 0xA1A2A3A4u;
    row.waystone_root = 0xB1B2B3B4u;
    row.use_count = 0xC1C2C3C4u;
    row.stmt_class = 0xD1;
    row.dir_depth = 0xE1;
    return row;
}

TEST(SysPatternRowTest, RoundTripsEveryField) {
    const SysPatternRow in = SampleRow();
    const auto bytes = in.Encode();

    auto out = SysPatternRow::Decode(bytes);
    ASSERT_TRUE(out.ok()) << out.status().message();

    EXPECT_EQ(out.value().oid, in.oid);
    EXPECT_EQ(out.value().pattern_id, in.pattern_id);
    EXPECT_EQ(out.value().last_seen, in.last_seen);
    EXPECT_EQ(out.value().fingerprint_version, in.fingerprint_version);
    EXPECT_EQ(out.value().waystone_root, in.waystone_root);
    EXPECT_EQ(out.value().use_count, in.use_count);
    EXPECT_EQ(out.value().stmt_class, in.stmt_class);
    EXPECT_EQ(out.value().dir_depth, in.dir_depth);
}

TEST(SysPatternRowTest, EveryFieldOccupiesItsOwnBytes) {
    // The bug a round-trip alone cannot catch: two fields sharing an
    // offset, or one overlapping the next. Encode a row that is zero
    // except for a single field, and nothing else may come back non-zero.
    const SysPatternRow sample = SampleRow();

    struct Probe {
        const char* name;
        SysPatternRow row;
    };
    std::vector<Probe> probes;
    {
        SysPatternRow r{}; r.oid = sample.oid;
        probes.push_back({"oid", r});
    }
    {
        SysPatternRow r{}; r.pattern_id = sample.pattern_id;
        probes.push_back({"pattern_id", r});
    }
    {
        SysPatternRow r{}; r.last_seen = sample.last_seen;
        probes.push_back({"last_seen", r});
    }
    {
        SysPatternRow r{}; r.fingerprint_version = sample.fingerprint_version;
        probes.push_back({"fingerprint_version", r});
    }
    {
        SysPatternRow r{}; r.waystone_root = sample.waystone_root;
        probes.push_back({"waystone_root", r});
    }
    {
        SysPatternRow r{}; r.use_count = sample.use_count;
        probes.push_back({"use_count", r});
    }
    {
        SysPatternRow r{}; r.stmt_class = sample.stmt_class;
        probes.push_back({"stmt_class", r});
    }
    {
        SysPatternRow r{}; r.dir_depth = sample.dir_depth;
        probes.push_back({"dir_depth", r});
    }

    for (const auto& probe : probes) {
        auto out = SysPatternRow::Decode(probe.row.Encode());
        ASSERT_TRUE(out.ok()) << probe.name;

        int non_zero = 0;
        non_zero += out.value().oid != 0;
        non_zero += out.value().pattern_id != 0;
        non_zero += out.value().last_seen != 0;
        non_zero += out.value().fingerprint_version != 0;
        non_zero += out.value().waystone_root != 0;
        non_zero += out.value().use_count != 0;
        non_zero += out.value().stmt_class != 0;
        non_zero += out.value().dir_depth != 0;
        EXPECT_EQ(non_zero, 1) << "setting " << probe.name << " disturbed another field";
    }
}

TEST(SysPatternRowTest, OnDiskLayoutIsPinned) {
    // A new relation with no existing files to be compatible with - so this
    // is not protecting existing data, it is fixing the layout *now* so
    // that a later accidental reorder is caught before there is data to
    // lose. Little-endian, packed, 41 bytes since CREATE PATTERN appended
    // `flags` and `origin` (the superblock version moved with it, which is
    // what stops an older file from mounting and then misreading this row).
    SysPatternRow row{};
    row.pattern_id = 0x1122334455667788ull;
    row.dir_depth = 0x2A;
    row.flags = 0xBEEF;
    row.origin = kOriginUser;
    const auto bytes = row.Encode();

    ASSERT_EQ(bytes.size(), 41u);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kPatternIdOffset]), 0x88);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kPatternIdOffset + 7]), 0x11);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kDirDepthOffset]), 0x2A);

    // The two appended fields, including the byte order of the u16 - the
    // field whose placement (before `origin`, not after) is what keeps
    // every offsetof assert in rows.hpp holding.
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFlagsOffset]), 0xEF);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFlagsOffset + 1]), 0xBE);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kOriginOffset]), kOriginUser);
}

TEST(SysPatternRowTest, OriginAndPinningRoundTripIndependently) {
    // They are separate fields on purpose: an operator may pin an
    // auto-registered pattern without re-declaring it, and a declared
    // pattern may be created unpinned. Both of those are unspellable if
    // pinning is folded into origin, so both are pinned here.
    SysPatternRow row = SampleRow();
    row.origin = kOriginAuto;
    row.flags = kPatternPinned;
    auto pinned_auto = SysPatternRow::Decode(row.Encode());
    ASSERT_TRUE(pinned_auto.ok());
    EXPECT_EQ(pinned_auto.value().origin, kOriginAuto);
    EXPECT_EQ(pinned_auto.value().flags & kPatternPinned, kPatternPinned);

    row.origin = kOriginUser;
    row.flags = 0;
    auto unpinned_user = SysPatternRow::Decode(row.Encode());
    ASSERT_TRUE(unpinned_user.ok());
    EXPECT_EQ(unpinned_user.value().origin, kOriginUser);
    EXPECT_EQ(unpinned_user.value().flags & kPatternPinned, 0);
}

TEST(SysPatternRowTest, DecodeRefusesAnythingButTheExactSize) {
    const auto bytes = SampleRow().Encode();
    const std::span<const std::byte> full(bytes);

    EXPECT_FALSE(SysPatternRow::Decode(full.first(full.size() - 1)).ok());
    EXPECT_FALSE(SysPatternRow::Decode(full.first(0)).ok());

    // One byte too many is refused as well, not silently truncated: an
    // over-long payload means the tuple was written by something that
    // disagrees about the format, which is exactly what the check is for.
    std::vector<std::byte> longer(bytes.begin(), bytes.end());
    longer.push_back(std::byte{0});
    auto out = SysPatternRow::Decode(longer);
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kCorruption);
}

// ---- The zero-value row ---------------------------------------------------

TEST(SysPatternRowTest, AZeroedRowIsUnusableRatherThanPlausible) {
    // What a never-written or zeroed catalog page decodes to. Every field
    // that gates behaviour has to read as "no" here, or a blank page
    // becomes a pattern the engine believes in.
    SysPatternRow row{};
    auto out = SysPatternRow::Decode(row.Encode());
    ASSERT_TRUE(out.ok());

    EXPECT_FALSE(parser::IsCurrentFingerprintVersion(out.value().fingerprint_version));
    EXPECT_FALSE(HasWaystoneDirectory(out.value()));
    EXPECT_EQ(out.value().stmt_class, kStmtClassUnclassified);
    EXPECT_EQ(out.value().use_count, 0u);
}

TEST(SysPatternRowTest, DirDepthAloneDecidesWhetherADirectoryExists) {
    SysPatternRow row{};

    // A root with no depth is not a directory - it is a half-written pair,
    // and reading it would walk an unknown number of levels.
    row.waystone_root = 4096;
    row.dir_depth = 0;
    EXPECT_FALSE(HasWaystoneDirectory(row));

    row.dir_depth = 1;
    EXPECT_TRUE(HasWaystoneDirectory(row));

    // And the reason depth carries the fact rather than the root: a zeroed
    // row's root reads as page 0, which is a valid-looking PageId (it is
    // the superblock). Nothing may treat that as "no directory" by
    // inspecting the root.
    row.waystone_root = 0;
    EXPECT_TRUE(HasWaystoneDirectory(row));
}

// ---- Where the row is anchored --------------------------------------------

TEST(SysPatternRowTest, CatalogConstantsDoNotCollide) {
    EXPECT_NE(kSysPatternsTable, kSysTablesTable);
    EXPECT_NE(kSysPatternsTable, kSysIndexesTable);
    EXPECT_LT(kSysPatternsTable, kUserOidStart);

    // The bootstrap pages are fixed ids handed to CreateAt(), never
    // allocated - so a collision with another catalog page or with the
    // first user page would be found at Bootstrap() time, in a failure
    // that says nothing about the cause.
    EXPECT_NE(kCatalogPagePatterns, kCatalogPageTypes);
    EXPECT_NE(kCatalogPagePatterns, kCatalogPageColumns);
    EXPECT_NE(kCatalogPagePatterns, kCatalogPageObjects);
    EXPECT_NE(kCatalogPagePatterns, kCatalogPageTables);
    EXPECT_NE(kCatalogPagePatterns, kCatalogPageIndexes);
    EXPECT_LT(kCatalogPagePatterns, 128u);  // kds::server::kFirstUserPageId
}

// ---- sys.tables ------------------------------------------------------
//
// Covered here rather than only through catalog_test.cpp for the reason
// this file's header gives: a round trip through the same code hides an
// offset collision. `owner_core` was appended past `varheap_page_id`, so
// the fields either side of it are the ones worth pinning.

SysTableRow SampleTableRow() {
    SysTableRow row{};
    row.oid = 0x0102030405060708ull;
    row.namespace_oid = 0x1112131415161718ull;
    SetName(row.name, "orders");
    row.desc_page_id = 0xA1A2A3A4u;
    row.clustered_type = ClusteredType::kBtree;
    row.next_id = 0x2122232425262728ull;
    row.varheap_page_id = 0xB1B2B3B4u;
    row.owner_core = 0xC1C2C3C4u;
    // kUnordered rather than the default, so a codec that dropped the field
    // fails the round trip instead of passing on a zero that happens to be
    // the right answer.
    row.key_order = KeyOrder::kUnordered;
    return row;
}

// ---- sys.indexes (docs/spec/index.md §12, workplan IX03) ---------------

SysIndexRow SampleIndexRow() {
    SysIndexRow row{};
    row.index_oid = 0x0102030405060708ull;
    row.table_oid = 0x1112131415161718ull;
    row.root_page_id = 0xA1A2A3A4u;
    row.key_width = 0xB1B2;
    row.entry_width = 0xC1C2;
    SetName(row.name, "orders_by_owner");
    row.nkeys = 2;
    row.ncovered = 3;
    row.flags = 0;
    row.reserved0 = 0;
    row.key_cols = {7, 9, 0, 0};
    row.covered_cols = {11, 13, 17, 0, 0, 0, 0, 0};
    return row;
}

TEST(SysIndexRowTest, RoundTripsEveryFieldIncludingBothColumnArrays) {
    const SysIndexRow in = SampleIndexRow();
    auto out = SysIndexRow::Decode(in.Encode());
    ASSERT_TRUE(out.ok()) << out.status().message();

    EXPECT_EQ(out.value().index_oid, in.index_oid);
    EXPECT_EQ(out.value().table_oid, in.table_oid);
    EXPECT_EQ(out.value().root_page_id, in.root_page_id);
    EXPECT_EQ(out.value().key_width, in.key_width);
    EXPECT_EQ(out.value().entry_width, in.entry_width);
    EXPECT_EQ(NameView(out.value().name), "orders_by_owner");
    EXPECT_EQ(out.value().nkeys, in.nkeys);
    EXPECT_EQ(out.value().ncovered, in.ncovered);
    EXPECT_EQ(out.value().key_cols, in.key_cols);
    EXPECT_EQ(out.value().covered_cols, in.covered_cols);
}

TEST(SysIndexRowTest, TheColumnArraysArePackedAtTheirDeclaredStride) {
    // Written element by element rather than as one block copy, so the
    // on-disk stride is sizeof(uint16_t) and not whatever the compiler chose
    // for the std::array. Moving one entry must move exactly two bytes.
    SysIndexRow row = SampleIndexRow();
    const auto baseline = row.Encode();

    row.key_cols[1] = 0;
    const auto zeroed = row.Encode();

    for (std::size_t i = 0; i < SysIndexRow::kOnDiskSize; ++i) {
        const std::size_t at = SysIndexRow::kKeyColsOffset + sizeof(std::uint16_t);
        const bool in_field = i >= at && i < at + sizeof(std::uint16_t);
        if (in_field) continue;
        EXPECT_EQ(baseline[i], zeroed[i]) << "byte " << i << " moved with key_cols[1]";
    }
}

TEST(SysIndexRowTest, ACountPastItsArrayIsCorruptionRatherThanAnOutOfBoundsRead) {
    // One corrupt byte would otherwise let every later reader index off the
    // end of the array, so it is caught at the one door these rows come
    // through.
    SysIndexRow row = SampleIndexRow();
    row.nkeys = static_cast<std::uint8_t>(kMaxIndexKeyColumns + 1);
    auto out = SysIndexRow::Decode(row.Encode());
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kCorruption);

    // Zero too: an index with no key column has nothing to order by.
    row = SampleIndexRow();
    row.nkeys = 0;
    EXPECT_FALSE(SysIndexRow::Decode(row.Encode()).ok());

    row = SampleIndexRow();
    row.ncovered = static_cast<std::uint8_t>(kMaxIndexCoveredColumns + 1);
    EXPECT_FALSE(SysIndexRow::Decode(row.Encode()).ok());
}

TEST(SysIndexRowTest, AWrongSizedPayloadIsRefusedNeverInterpreted) {
    const auto encoded = SampleIndexRow().Encode();
    std::vector<std::byte> short_row(encoded.begin(), encoded.end() - 1);
    auto out = SysIndexRow::Decode(short_row);
    EXPECT_FALSE(out.ok());
}

TEST(SysTableRowTest, RoundTripsEveryFieldIncludingTheOwnerCore) {
    const SysTableRow in = SampleTableRow();
    auto out = SysTableRow::Decode(in.Encode());
    ASSERT_TRUE(out.ok()) << out.status().message();

    EXPECT_EQ(out.value().oid, in.oid);
    EXPECT_EQ(out.value().namespace_oid, in.namespace_oid);
    EXPECT_EQ(NameView(out.value().name), "orders");
    EXPECT_EQ(out.value().desc_page_id, in.desc_page_id);
    EXPECT_EQ(out.value().clustered_type, in.clustered_type);
    EXPECT_EQ(out.value().next_id, in.next_id);
    EXPECT_EQ(out.value().varheap_page_id, in.varheap_page_id);
    EXPECT_EQ(out.value().owner_core, in.owner_core);
    EXPECT_EQ(out.value().key_order, in.key_order);
}

TEST(SysTableRowTest, TheKeyOrderOccupiesItsOwnByte) {
    // Same check `owner_core` gets, for the same reason: a round trip
    // through one codec cannot catch an offset that overlaps a neighbour,
    // because both halves make the same mistake.
    SysTableRow row = SampleTableRow();
    const auto baseline = row.Encode();

    row.key_order = KeyOrder::kAscending;
    const auto zeroed = row.Encode();

    for (std::size_t i = 0; i < SysTableRow::kOnDiskSize; ++i) {
        if (i == SysTableRow::kKeyOrderOffset) continue;
        EXPECT_EQ(baseline[i], zeroed[i]) << "key_order disturbed byte " << i;
    }
}

TEST(SysTableRowTest, TheOwnerCoreOccupiesItsOwnBytes) {
    // Changing it must move nothing else - the check that catches an
    // offset overlap, which a round trip through one codec cannot.
    SysTableRow row = SampleTableRow();
    const auto baseline = row.Encode();

    row.owner_core = 0;
    const auto zeroed = row.Encode();

    for (std::size_t i = 0; i < SysTableRow::kOnDiskSize; ++i) {
        const bool in_field = i >= SysTableRow::kOwnerCoreOffset &&
                              i < SysTableRow::kOwnerCoreOffset + sizeof(std::uint32_t);
        if (in_field) continue;
        EXPECT_EQ(baseline[i], zeroed[i]) << "owner_core disturbed byte " << i;
    }
}

TEST(SysTableRowTest, OnDiskLayoutIsPinned) {
    // The row grew by four bytes for `owner_core`, one for the key-order byte (`key_mode` when it was added), and
    // four for `anchor_page_id` (PW2-1), each a format-version event - the
    // superblock bumps to 10, 14 and 15 are the other halves. Pinned so
    // the next person to add a field cannot do so quietly.
    EXPECT_EQ(SysTableRow::kOwnerCoreOffset,
              SysTableRow::kVarHeapPageIdOffset + sizeof(PageId));
    EXPECT_EQ(SysTableRow::kKeyOrderOffset,
              SysTableRow::kOwnerCoreOffset + sizeof(std::uint32_t));
    EXPECT_EQ(SysTableRow::kAnchorPageIdOffset,
              SysTableRow::kKeyOrderOffset + sizeof(std::uint8_t));
    EXPECT_EQ(SysTableRow::kOnDiskSize,
              SysTableRow::kAnchorPageIdOffset + sizeof(PageId));
}

TEST(SysTableRowTest, DecodeRefusesAnythingButTheExactSize) {
    const auto bytes = SampleTableRow().Encode();
    std::vector<std::byte> short_row(bytes.begin(), bytes.end() - 1);
    std::vector<std::byte> long_row(bytes.begin(), bytes.end());
    long_row.push_back(std::byte{0});

    EXPECT_FALSE(SysTableRow::Decode(short_row).ok());
    EXPECT_FALSE(SysTableRow::Decode(long_row).ok());
}

// ---- Placement (core_placement.hpp) ----------------------------------

TEST(CorePlacementTest, TheDefaultPolicyOwnsARelationByItsCreatingCore) {
    // The invariant placement has to satisfy: ownership names the core that
    // may run statements against a relation, and a relation owned by a core
    // that cannot reach its pages is not a placement, it is an unreachable
    // relation. Under the default policy that means the creating core,
    // whatever the count and whatever the sequence.
    for (std::uint32_t cores : {1u, 2u, 4u}) {
        for (std::uint64_t seq = 0; seq < 8; ++seq) {
            EXPECT_EQ(AssignOwnerCore(PlacementPolicy::kCreatingCore, kSystemCore, cores, seq),
                      kSystemCore);
        }
    }
}

TEST(CorePlacementTest, TheRotationIsOptInAndNeverTheDefault) {
    // M1's round-robin was once applied unconditionally, from P0 until the
    // affinity guard existed, and every statement on a two-core instance
    // failed - placement said core 1, execution ran on core 0. Since CC7
    // the rotation is *legal* (the publish handoff grants the owner fault
    // rights, workplan P6b/P6c) but statements still all run on core 0, so
    // it stays behind the `placement = rotate` key and the default answers
    // exactly as it did before the policy existed.
    EXPECT_EQ(AssignOwnerCore(PlacementPolicy::kCreatingCore, kSystemCore, 4, 0), kSystemCore);
    EXPECT_EQ(AssignOwnerCore(PlacementPolicy::kRotate, kSystemCore, 4, 0), 1u);
    EXPECT_EQ(AssignOwnerCore(PlacementPolicy::kRotate, kSystemCore, 4, 1), 2u);
}

// ---- decimal(p, s) packed into `len` (TY02) -----------------------------
//
// The packing exists so that adding DECIMAL cost **no data-file format
// change**: `SysColumnRow` has no spare byte, so widening it would have
// meant a superblock version bump and every pre-existing file refusing to
// mount. `len` was already dead weight for a decimal column - only `char`
// reads it as a width - so the pair rides in it.

TEST(SysColumnRowDecimalTest, PrecisionAndScaleRoundTripThroughLen) {
    for (std::uint8_t p = 1; p <= 18; ++p) {
        for (std::uint8_t s = 0; s <= p; ++s) {
            const std::uint32_t packed = PackDecimalLen(p, s);
            EXPECT_EQ(DecimalPrecisionOf(packed), p) << int(p) << "," << int(s);
            EXPECT_EQ(DecimalScaleOf(packed), s) << int(p) << "," << int(s);
        }
    }
}

TEST(SysColumnRowDecimalTest, ThePackingSurvivesTheRowsOwnEncoding) {
    // The point of the whole exercise: it goes to disk in a field that
    // already existed, so it round-trips through Encode/Decode unchanged
    // and no format version moved.
    SysColumnRow row{};
    row.oid = 7;
    row.rel_id = 4000;
    row.pos = 2;
    SetName(row.name, "price");
    row.type_val = kTypeValDecimal;
    row.len = PackDecimalLen(10, 2);
    row.notnull = true;

    auto decoded = SysColumnRow::Decode(row.Encode());
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().len, row.len);
    EXPECT_EQ(DecimalPrecisionOf(decoded.value().len), 10);
    EXPECT_EQ(DecimalScaleOf(decoded.value().len), 2);
}

TEST(SysColumnRowDecimalTest, TheUnusedHighBitsStayZero) {
    // Sixteen of len's thirty-two bits are used, and the rest are left
    // available rather than filled with anything - which is what makes a
    // future third field in here possible without a format event.
    EXPECT_EQ(PackDecimalLen(18, 18) >> 16, 0u);
}

TEST(SysColumnRowDecimalTest, ColumnTypeTextRendersTheDeclaredForm) {
    // The client-visible half: `len` is no longer readable as a width
    // without knowing the type, so the two display paths render the type.
    SysColumnRow dec{};
    dec.type_val = kTypeValDecimal;
    dec.len = PackDecimalLen(10, 2);
    EXPECT_EQ(ColumnTypeText(dec, "decimal"), "decimal(10,2)");

    SysColumnRow chr{};
    chr.type_val = kTypeValChar;
    chr.len = 8;
    EXPECT_EQ(ColumnTypeText(chr, "char"), "char(8)");

    // Every other type's width comes from its type_val, so the bare name
    // is the whole truth and `len` says nothing a reader wants.
    SysColumnRow i64{};
    i64.type_val = kTypeValInt64;
    i64.len = 8;
    EXPECT_EQ(ColumnTypeText(i64, "int64"), "int64");

    SysColumnRow date{};
    date.type_val = kTypeValDate;
    date.len = 4;
    EXPECT_EQ(ColumnTypeText(date, "date"), "date");
}

}  // namespace
}  // namespace kds::catalog
