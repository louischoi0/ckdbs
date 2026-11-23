#include "kds/catalog/rows.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

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
    row.fetch_id = 0x1122334455667788ull;
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
    EXPECT_EQ(out.value().fetch_id, in.fetch_id);
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
        SysPatternRow r{}; r.fetch_id = sample.fetch_id;
        probes.push_back({"fetch_id", r});
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
        non_zero += out.value().fetch_id != 0;
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
    // **Still 41 after that feature was withdrawn on 2026-08-31**, and after
    // CF-S1b re-scoped bit 0 of `flags` as the Waystone opt-in and `origin`
    // as a reserved byte, written 0: the codec still round-trips whatever
    // the byte holds, so a layout change is caught here. **178 since
    // superblock 38** (DI-S3): the binding's namespace and 16 relations
    // after `last_seen`, its count after `origin`.
    SysPatternRow row{};
    row.fetch_id = 0x1122334455667788ull;
    row.dir_depth = 0x2A;
    row.flags = 0xBEEF;
    row.origin = 0x5A;
    row.binding_namespace = 0x0102030405060708ull;
    row.binding_oids[0] = 0x1112131415161718ull;
    row.binding_oids[15] = 0x2122232425262728ull;
    row.binding_n = 16;
    const auto bytes = row.Encode();

    ASSERT_EQ(bytes.size(), 178u);
    EXPECT_EQ(SysPatternRow::kBindingNamespaceOffset, 24u);
    EXPECT_EQ(SysPatternRow::kBindingOidsOffset, 32u);
    EXPECT_EQ(SysPatternRow::kFingerprintVersionOffset, 160u);
    EXPECT_EQ(SysPatternRow::kBindingCountOffset, 177u);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kBindingNamespaceOffset]), 0x08);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kBindingOidsOffset]), 0x18);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kBindingOidsOffset + 15 * 8]), 0x28);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kBindingCountOffset]), 16);
    const auto decoded = SysPatternRow::Decode(bytes);
    ASSERT_TRUE(decoded.ok());
    const kds::stats::StatementBinding binding = BindingOf(decoded.value());
    EXPECT_EQ(binding.namespace_oid, 0x0102030405060708ull);
    EXPECT_EQ(binding.n, 16u);
    EXPECT_EQ(binding.oids[15], 0x2122232425262728ull);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFetchIdOffset]), 0x88);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFetchIdOffset + 7]), 0x11);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kDirDepthOffset]), 0x2A);

    // The two appended fields, including the byte order of the u16 - the
    // field whose placement (before `origin`, not after) is what keeps
    // every offsetof assert in rows.hpp holding.
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFlagsOffset]), 0xEF);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kFlagsOffset + 1]), 0xBE);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kOriginOffset]), 0x5A);
}

// CF-S1b: bit 0 of `flags` is the opt-in, and the reserved `origin` byte
// beside it does not move with it.
TEST(SysPatternRowTest, TheWaystoneBitRoundTripsBesideTheReservedByte) {
    SysPatternRow row = SampleRow();
    row.origin = 0;
    row.flags = kPatternWaystone;
    auto opted = SysPatternRow::Decode(row.Encode());
    ASSERT_TRUE(opted.ok());
    EXPECT_EQ(opted.value().origin, 0);
    EXPECT_EQ(opted.value().flags & kPatternWaystone, kPatternWaystone);

    row.flags = 0;
    auto cleared = SysPatternRow::Decode(row.Encode());
    ASSERT_TRUE(cleared.ok());
    EXPECT_EQ(cleared.value().origin, 0);
    EXPECT_EQ(cleared.value().flags & kPatternWaystone, 0);
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

    // The page half of this test is gone: it hand-checked five of what are
    // now twelve roots, and catalog.cpp's static_assert over
    // well_known.hpp's `CatalogRootsAreDistinctAndFixed()` covers all of
    // them - distinctness and the sub-user-range bound - at compile time,
    // over the one list every root already joins.
}

// ---- sys.tables ------------------------------------------------------
//
// Covered here rather than only through catalog_test.cpp for the reason
// this file's header gives: a round trip through the same code hides an
// offset collision. The embedded key's columns sit past `varheap_page_id`
// (`kEmbeddedKeyColsOffset`, where the retired `owner_core` word was), so
// the fields either side of them are the ones worth pinning.

SysTableRow SampleTableRow() {
    SysTableRow row{};
    row.oid = 0x0102030405060708ull;
    row.namespace_oid = 0x1112131415161718ull;
    row.desc_page_id = 0xA1A2A3A4u;
    row.clustered_type = ClusteredType::kBtree;
    row.next_id = 0x2122232425262728ull;
    row.varheap_page_id = 0xB1B2B3B4u;
    // An embedded key (DR-S3e, superblock 41) rather than the default form,
    // so a codec that dropped the form or its columns fails the round trip
    // instead of passing on zeros that happen to be the right answer.
    row.key_form = kKeyFormEmbedded;
    row.embedded_key_cols = {3, 1, 0, 0};
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

// CI-S1: the building bit round-trips, and a bit nothing defines is
// refused at the one door the rows come through.
TEST(SysIndexRowTest, TheBuildingFlagRoundTripsAndAnUndefinedBitIsCorruption) {
    SysIndexRow in = SampleIndexRow();
    in.flags = kIndexFlagBuilding;
    auto out = SysIndexRow::Decode(in.Encode());
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().flags, kIndexFlagBuilding);

    // DN: a heap's Keystone index, unique, round-trips too (superblock 39).
    in.flags = kIndexFlagUnique | kIndexFlagKeystone;
    auto keystone = SysIndexRow::Decode(in.Encode());
    ASSERT_TRUE(keystone.ok()) << keystone.status().message();
    EXPECT_EQ(keystone.value().flags, kIndexFlagUnique | kIndexFlagKeystone);

    for (const std::uint8_t refused_flags :
         {std::uint8_t{0x8}, std::uint8_t(kIndexFlagUnique | kIndexFlagBuilding)}) {
        in.flags = refused_flags;
        auto refused = SysIndexRow::Decode(in.Encode());
        ASSERT_FALSE(refused.ok()) << int(refused_flags);
        EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);
    }
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

TEST(SysTableRowTest, RoundTripsEveryField) {
    const SysTableRow in = SampleTableRow();
    auto out = SysTableRow::Decode(in.Encode());
    ASSERT_TRUE(out.ok()) << out.status().message();

    EXPECT_EQ(out.value().oid, in.oid);
    EXPECT_EQ(out.value().namespace_oid, in.namespace_oid);
    EXPECT_EQ(out.value().desc_page_id, in.desc_page_id);
    EXPECT_EQ(out.value().clustered_type, in.clustered_type);
    EXPECT_EQ(out.value().next_id, in.next_id);
    EXPECT_EQ(out.value().varheap_page_id, in.varheap_page_id);
    EXPECT_EQ(out.value().key_form, in.key_form);
    EXPECT_EQ(out.value().embedded_key_cols, in.embedded_key_cols);
}

TEST(SysTableRowTest, TheKeyFormAndItsColumnsOccupyTheirOwnBytes) {
    // A round trip through one codec cannot catch an offset that overlaps a
    // neighbour, because both halves make the same mistake: so the bytes
    // that differ between two rows are counted here.
    SysTableRow row = SampleTableRow();
    const auto baseline = row.Encode();

    row.key_form = kKeyFormIssued;
    row.embedded_key_cols = {};
    const auto issued = row.Encode();

    for (std::size_t i = 0; i < SysTableRow::kOnDiskSize; ++i) {
        const bool theirs = i == SysTableRow::kKeyFormOffset ||
                            (i >= SysTableRow::kEmbeddedKeyColsOffset &&
                             i < SysTableRow::kEmbeddedKeyColsOffset + kMaxEmbeddedKeyColumns);
        if (theirs) continue;
        EXPECT_EQ(baseline[i], issued[i]) << "the key form disturbed byte " << i;
    }
    EXPECT_EQ(baseline[SysTableRow::kKeyFormOffset], std::byte{kKeyFormEmbedded});
    EXPECT_EQ(baseline[SysTableRow::kEmbeddedKeyColsOffset], std::byte{3});
    EXPECT_EQ(baseline[SysTableRow::kEmbeddedKeyColsOffset + 1], std::byte{1});
    // A row from before superblock 41 has both written 0: no embedded key.
    auto old = SysTableRow::Decode(issued);
    ASSERT_TRUE(old.ok()) << old.status().message();
    EXPECT_EQ(old.value().key_form, kKeyFormIssued);
    EXPECT_EQ(old.value().embedded_key_cols, (std::array<std::uint8_t, 4>{}));
}

// DR-R12: a form this build does not know, or key columns without the form
// that carries them, is a row this build never wrote - refused, not read as
// the nearest form it knows.
TEST(SysTableRowTest, AKeyFormThisBuildNeverWritesIsCorruption) {
    const SysTableRow in = SampleTableRow();
    auto unknown = in.Encode();
    unknown[SysTableRow::kKeyFormOffset] = std::byte{2};
    EXPECT_EQ(SysTableRow::Decode(unknown).status().code(), StatusCode::kCorruption);

    auto columns_without_form = in.Encode();
    columns_without_form[SysTableRow::kKeyFormOffset] = std::byte{kKeyFormIssued};
    EXPECT_EQ(SysTableRow::Decode(columns_without_form).status().code(),
              StatusCode::kCorruption);

    auto form_without_columns = in.Encode();
    for (std::size_t i = 0; i < kMaxEmbeddedKeyColumns; ++i) {
        form_without_columns[SysTableRow::kEmbeddedKeyColsOffset + i] = std::byte{0};
    }
    EXPECT_EQ(SysTableRow::Decode(form_without_columns).status().code(),
              StatusCode::kCorruption);
}

TEST(SysTableRowTest, TheRetiredNameIsWrittenZeroAndNeverRead) {
    // The relation's name is its sys.objects row's alone; the 64 bytes that
    // held a copy here stay, written 0, and the stale name a volume written
    // before still holds in them changes nothing the decoder returns.
    const SysTableRow in = SampleTableRow();
    auto bytes = in.Encode();
    for (std::size_t i = 0; i < kCatalogNameMax; ++i) {
        EXPECT_EQ(bytes[SysTableRow::kRetiredNameOffset + i], std::byte{0}) << "byte " << i;
        bytes[SysTableRow::kRetiredNameOffset + i] = std::byte{'o'};
    }
    auto out = SysTableRow::Decode(bytes);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().Encode(), in.Encode()) << "the old name leaked into the decoded row";
}

TEST(SysTableRowTest, OnDiskLayoutIsPinned) {
    // The row grew by four bytes for `owner_core` (retired at AT-S9; the
    // embedded key's columns since superblock 41), one for the key-order
    // byte (`key_mode` when it was added; the key form since 41), and four
    // for `anchor_page_id` (PW2-1), and two for `spare_slots` (BW-R9), each
    // a format-version event - the superblock bumps to 10, 14, 15, 25 and
    // 41 are the other halves.
    // Pinned so the next person to add a field cannot do so quietly.
    EXPECT_EQ(SysTableRow::kEmbeddedKeyColsOffset,
              SysTableRow::kVarHeapPageIdOffset + sizeof(PageId));
    EXPECT_EQ(SysTableRow::kKeyFormOffset,
              SysTableRow::kEmbeddedKeyColsOffset + sizeof(std::uint32_t));
    EXPECT_EQ(SysTableRow::kAnchorPageIdOffset,
              SysTableRow::kKeyFormOffset + sizeof(std::uint8_t));
    EXPECT_EQ(SysTableRow::kSpareSlotsOffset,
              SysTableRow::kAnchorPageIdOffset + sizeof(PageId));
    EXPECT_EQ(SysTableRow::kOnDiskSize,
              SysTableRow::kSpareSlotsOffset + sizeof(std::uint16_t));
}

TEST(SysTableRowTest, TheSpareSlotCountRoundTrips) {
    SysTableRow in = SampleTableRow();
    in.spare_slots = 64;
    auto out = SysTableRow::Decode(in.Encode());
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().spare_slots, 64u);
}

TEST(SysColumnRowTest, TheFlagsByteRoundTripsAfterTheCabinPolicy) {
    SysColumnRow in{};
    in.cabin_policy = 3;
    in.flags = SysColumnRow::kColumnDropped | SysColumnRow::kColumnInSlot;
    const auto bytes = in.Encode();
    EXPECT_EQ(std::to_integer<int>(bytes[SysColumnRow::kFlagsOffset]), 0x03);
    EXPECT_EQ(SysColumnRow::kFlagsOffset, SysColumnRow::kCabinPolicyOffset + 1);
    auto out = SysColumnRow::Decode(bytes);
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().dropped());
    EXPECT_TRUE(out.value().in_slot());
    EXPECT_EQ(out.value().cabin_policy, 3);
}

// CD-S3: the clean flag beside the drop, and the generation word after the
// flags byte at its own unaligned offset; clean without dropped is refused.
TEST(SysColumnRowTest, TheCleanFlagAndTheGenerationRoundTripAndCleanNeedsDropped) {
    SysColumnRow in{};
    in.flags = SysColumnRow::kColumnDropped | SysColumnRow::kColumnInSlot |
               SysColumnRow::kColumnSlotClean;
    in.generation = 0x01020304;
    const auto bytes = in.Encode();
    EXPECT_EQ(std::to_integer<int>(bytes[SysColumnRow::kFlagsOffset]), 0x0B);
    EXPECT_EQ(std::to_integer<int>(bytes[SysColumnRow::kGenerationOffset]), 0x04);
    auto out = SysColumnRow::Decode(bytes);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_TRUE(out.value().slot_clean());
    EXPECT_EQ(out.value().generation, 0x01020304u);

    in.flags = SysColumnRow::kColumnInSlot | SysColumnRow::kColumnSlotClean;
    auto refused = SysColumnRow::Decode(in.Encode());
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);
}

TEST(SysTableRowTest, DecodeRefusesAnythingButTheExactSize) {
    const auto bytes = SampleTableRow().Encode();
    std::vector<std::byte> short_row(bytes.begin(), bytes.end() - 1);
    std::vector<std::byte> long_row(bytes.begin(), bytes.end());
    long_row.push_back(std::byte{0});

    EXPECT_FALSE(SysTableRow::Decode(short_row).ok());
    EXPECT_FALSE(SysTableRow::Decode(long_row).ok());
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

    // A varchar renders a width only when the column declared one. `len` of
    // 0 is not "unset": it is the instance's kds.inline_cell_width, which is
    // what every varchar column carried before `varchar(N)` existed - and
    // printing that number here would report a column property the column
    // does not have.
    SysColumnRow var{};
    var.type_val = kTypeValVarchar;
    var.len = 32;
    EXPECT_EQ(ColumnTypeText(var, "varchar"), "varchar(32)");

    SysColumnRow legacy{};
    legacy.type_val = kTypeValVarchar;
    legacy.len = 0;
    EXPECT_EQ(ColumnTypeText(legacy, "varchar"), "varchar");

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
