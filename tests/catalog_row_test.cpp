#include "kds/catalog/rows.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"

// Pure codec tests for sys.patterns rows (docs/waystone-concpets.md
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
    // lose. Little-endian, packed, 38 bytes.
    SysPatternRow row{};
    row.pattern_id = 0x1122334455667788ull;
    row.dir_depth = 0x2A;
    const auto bytes = row.Encode();

    ASSERT_EQ(bytes.size(), 38u);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kPatternIdOffset]), 0x88);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kPatternIdOffset + 7]), 0x11);
    EXPECT_EQ(std::to_integer<int>(bytes[SysPatternRow::kDirDepthOffset]), 0x2A);
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

}  // namespace
}  // namespace kds::catalog
