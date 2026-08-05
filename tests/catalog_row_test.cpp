#include "kds/catalog/rows.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/core_placement.hpp"
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
    return row;
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
    // The row grew by four bytes, which is a format-version event - the
    // superblock bump to 10 is the other half of it. Pinned so the next
    // person to add a field cannot do so quietly.
    EXPECT_EQ(SysTableRow::kOwnerCoreOffset,
              SysTableRow::kVarHeapPageIdOffset + sizeof(PageId));
    EXPECT_EQ(SysTableRow::kOnDiskSize,
              SysTableRow::kOwnerCoreOffset + sizeof(std::uint32_t));
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

TEST(CorePlacementTest, ASingleCoreInstancePutsEverythingOnTheSystemCore) {
    for (std::uint64_t seq = 0; seq < 8; ++seq) {
        EXPECT_EQ(AssignOwnerCore(/*core_count=*/1, seq), kSystemCore);
    }
}

TEST(CorePlacementTest, UserRelationsRotateOverTheNonSystemCores) {
    // The `[PROPOSED]` round-robin of M1. Nothing may depend on this
    // distribution - only on ownership being recorded - but the policy that
    // exists should be the one that was described.
    EXPECT_EQ(AssignOwnerCore(4, 0), 1u);
    EXPECT_EQ(AssignOwnerCore(4, 1), 2u);
    EXPECT_EQ(AssignOwnerCore(4, 2), 3u);
    EXPECT_EQ(AssignOwnerCore(4, 3), 1u);
}

TEST(CorePlacementTest, TheSystemCoreNeverOwnsAUserRelationWhenThereIsAnywhereElse) {
    // M5: core 0 already owns the superblock, the free map, file growth and
    // the catalog pages. A user relation on it is the one core everybody
    // else queues behind.
    for (std::uint32_t cores = 2; cores <= 8; ++cores) {
        for (std::uint64_t seq = 0; seq < 20; ++seq) {
            const std::uint32_t owner = AssignOwnerCore(cores, seq);
            EXPECT_NE(owner, kSystemCore) << "cores=" << cores << " seq=" << seq;
            EXPECT_LT(owner, cores);
        }
    }
}

}  // namespace
}  // namespace kds::catalog
