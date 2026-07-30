#include "kds/stats/waystone_dir.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/stats/waystone.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The page directory (waystone-concpets.md section 6, spec test 12-1).
// Three properties carry the design and each has its own group below:
// the walk lands on the right leaf including across level boundaries,
// unpopulated ranges cost nothing, and deepening the directory does not
// move anything already in it.

namespace kds::stats {
namespace {

// Coverage boundaries, spelled out rather than inlined so a failure names
// which one broke. Depth 1 covers 2^19 tuples, depth 2 covers 2^30.
constexpr std::uint64_t kDepth1Coverage = 524288;             // 2048 * 256
constexpr std::uint64_t kDepth2Coverage = 1073741824ull;      // 2048^2 * 256

class WaystoneDirTest : public ::testing::Test {
protected:
    // A leaf reached through the directory, with the entry actually
    // written into it - the round trip a hook and a probe make.
    Status PutEntry(PageId root, int depth, std::uint64_t pk) {
        auto leaf = LookupOrCreateEntryPage(store_, root, depth, pk);
        if (!leaf.ok()) return leaf.status();

        auto bytes = store_.Get(leaf.value());
        if (!bytes.ok()) return bytes.status();

        WaystoneEntry entry;
        entry.pk = pk;
        entry.page_id = 1000;
        entry.slot = 0;
        entry.flags = kEntryLive;
        entry.use_count = 0;
        entry.last_ts = 0;
        entry.page_epoch = 0;
        entry.reserved = 0;
        return WriteEntry(bytes.value(), EntrySlotOf(pk), entry);
    }

    StatusOr<WaystoneEntry> GetEntry(PageId root, int depth, std::uint64_t pk) {
        auto leaf = LookupEntryPage(store_, root, depth, pk);
        if (!leaf.ok()) return leaf.status();
        if (leaf.value() == kInvalidPageId) {
            return Status::NotFound("no entry page for pk " + std::to_string(pk));
        }
        auto bytes = store_.Get(leaf.value());
        if (!bytes.ok()) return bytes.status();
        return ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()), EntrySlotOf(pk));
    }

    storage::InMemoryPageStore store_{1};
};

// ---- Depth selection ----------------------------------------------------

TEST_F(WaystoneDirTest, DepthIsTheSmallestThatCoversThePk) {
    EXPECT_EQ(DirDepthFor(0).value(), 1);
    EXPECT_EQ(DirDepthFor(kDepth1Coverage - 1).value(), 1);
    EXPECT_EQ(DirDepthFor(kDepth1Coverage).value(), 2);
    EXPECT_EQ(DirDepthFor(kDepth2Coverage - 1).value(), 2);
    EXPECT_EQ(DirDepthFor(kDepth2Coverage).value(), 3);
    EXPECT_EQ(DirDepthFor(kMaxPk).value(), 3);
}

TEST_F(WaystoneDirTest, APkWiderThanTheKeystoneRangeHasNoDepth) {
    EXPECT_EQ(DirDepthFor(kMaxPk + 1).status().code(), StatusCode::kInvalidArgument);
}

// ---- The walk -----------------------------------------------------------

TEST_F(WaystoneDirTest, RoundTripsAtEveryDepth) {
    for (int depth = 1; depth <= kMaxDirDepth; ++depth) {
        auto root = CreateDirPage(store_);
        ASSERT_TRUE(root.ok()) << root.status().message();

        const std::uint64_t pk = (depth == 1) ? 300 : DirCoverageAtDepth(depth - 1) + 17;
        ASSERT_TRUE(PutEntry(root.value(), depth, pk).ok()) << "depth " << depth;

        auto found = GetEntry(root.value(), depth, pk);
        ASSERT_TRUE(found.ok()) << "depth " << depth << ": " << found.status().message();
        EXPECT_EQ(found.value().pk, pk);
        EXPECT_EQ(found.value().flags & kEntryLive, kEntryLive);
    }
}

TEST_F(WaystoneDirTest, PksOnEitherSideOfTheDepth1BoundaryDoNotCollide) {
    // 524,287 -> 524,288 is the pk where a depth-1 directory runs out and
    // the spec calls the boundary out by name.
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    ASSERT_TRUE(PutEntry(root.value(), 2, kDepth1Coverage - 1).ok());
    ASSERT_TRUE(PutEntry(root.value(), 2, kDepth1Coverage).ok());

    EXPECT_EQ(GetEntry(root.value(), 2, kDepth1Coverage - 1).value().pk, kDepth1Coverage - 1);
    EXPECT_EQ(GetEntry(root.value(), 2, kDepth1Coverage).value().pk, kDepth1Coverage);
}

TEST_F(WaystoneDirTest, ManyPksAcrossWidelySeparatedRangesAllResolve) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    const std::vector<std::uint64_t> pks = {
        0, 1, 255, 256, 4095, 4096, kDepth1Coverage - 1, kDepth1Coverage,
        kDepth1Coverage + 1, 1u << 20, 1u << 25, kDepth2Coverage - 1,
    };
    for (const std::uint64_t pk : pks) {
        ASSERT_TRUE(PutEntry(root.value(), 3, pk).ok()) << "pk " << pk;
    }
    for (const std::uint64_t pk : pks) {
        auto found = GetEntry(root.value(), 3, pk);
        ASSERT_TRUE(found.ok()) << "pk " << pk << ": " << found.status().message();
        EXPECT_EQ(found.value().pk, pk);
    }
}

TEST_F(WaystoneDirTest, APkThePkSpaceCoversButTheDepthDoesNotIsRefusedNotAliased) {
    // The failure this guards against is silent: the walk masks each digit
    // to 11 bits, so an over-wide pk would drop its high bits and land on
    // some other pk's entry.
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    EXPECT_EQ(LookupEntryPage(store_, root.value(), 1, kDepth1Coverage).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(LookupOrCreateEntryPage(store_, root.value(), 1, kDepth1Coverage).status().code(),
              StatusCode::kInvalidArgument);
}

TEST_F(WaystoneDirTest, ADepthOutsideTheLegalRangeIsRefused) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    EXPECT_EQ(LookupEntryPage(store_, root.value(), 0, 1).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(LookupEntryPage(store_, root.value(), kMaxDirDepth + 1, 1).status().code(),
              StatusCode::kInvalidArgument);
}

// ---- Lazy allocation ----------------------------------------------------

TEST_F(WaystoneDirTest, AnUnpopulatedRangeIsAMissNotAnError) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    auto leaf = LookupEntryPage(store_, root.value(), 2, 12345);
    ASSERT_TRUE(leaf.ok()) << "a miss is a normal answer on the probe path";
    EXPECT_EQ(leaf.value(), kInvalidPageId);
}

TEST_F(WaystoneDirTest, LookupNeverAllocates) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    const std::size_t before = store_.page_count();

    for (std::uint64_t pk = 0; pk < 100000; pk += 4096) {
        ASSERT_TRUE(LookupEntryPage(store_, root.value(), 2, pk).ok());
    }
    EXPECT_EQ(store_.page_count(), before) << "a read-only walk must not grow the directory";
}

TEST_F(WaystoneDirTest, ASparseIdSpaceCostsOnlyWhatItTouches) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    // Two pks in the same leaf, then one very far away. At depth 2 the
    // first two share their whole path; the third shares only the root.
    ASSERT_TRUE(PutEntry(root.value(), 2, 10).ok());
    const std::size_t after_first = store_.page_count();

    ASSERT_TRUE(PutEntry(root.value(), 2, 11).ok());
    EXPECT_EQ(store_.page_count(), after_first)
        << "a pk in an already-allocated leaf must allocate nothing";

    ASSERT_TRUE(PutEntry(root.value(), 2, kDepth1Coverage + 10).ok());
    EXPECT_GT(store_.page_count(), after_first);

    // And nothing in between was materialized: a dense directory over that
    // gap would be thousands of pages.
    EXPECT_LT(store_.page_count(), 10u);
}

// ---- Depth growth -------------------------------------------------------

TEST_F(WaystoneDirTest, GrowingPreservesEveryPriorMapping) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    const std::vector<std::uint64_t> pks = {0, 255, 256, 1000, kDepth1Coverage - 1};
    for (const std::uint64_t pk : pks) {
        ASSERT_TRUE(PutEntry(root.value(), 1, pk).ok());
    }

    auto grown = GrowDirectory(store_, root.value(), 1);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    EXPECT_NE(grown.value(), root.value());

    // Same pks, one level deeper, same answers.
    for (const std::uint64_t pk : pks) {
        auto found = GetEntry(grown.value(), 2, pk);
        ASSERT_TRUE(found.ok()) << "pk " << pk << " lost by growth: " << found.status().message();
        EXPECT_EQ(found.value().pk, pk);
    }
}

TEST_F(WaystoneDirTest, GrowingCostsOnePage) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(PutEntry(root.value(), 1, 42).ok());
    const std::size_t before = store_.page_count();

    auto grown = GrowDirectory(store_, root.value(), 1);
    ASSERT_TRUE(grown.ok());
    EXPECT_EQ(store_.page_count(), before + 1)
        << "growth is a root relink, not a rebuild";
}

TEST_F(WaystoneDirTest, AGrownDirectoryAddressesTheRangeThatForcedTheGrowth) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(PutEntry(root.value(), 1, 7).ok());

    auto grown = GrowDirectory(store_, root.value(), 1);
    ASSERT_TRUE(grown.ok());

    // The pk depth 1 could not reach now resolves, and the old one still does.
    ASSERT_TRUE(PutEntry(grown.value(), 2, kDepth1Coverage + 5).ok());
    EXPECT_EQ(GetEntry(grown.value(), 2, kDepth1Coverage + 5).value().pk, kDepth1Coverage + 5);
    EXPECT_EQ(GetEntry(grown.value(), 2, 7).value().pk, 7u);
}

TEST_F(WaystoneDirTest, GrowingPastTheMaximumDepthIsRefused) {
    auto root = CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    EXPECT_EQ(GrowDirectory(store_, root.value(), kMaxDirDepth).status().code(),
              StatusCode::kOutOfRange);
}

// ---- Digit extraction ---------------------------------------------------

TEST(WaystoneDirIndexTest, DigitsAreMostSignificantFirst) {
    // pk 2^20 at depth 2: logical = 2^20 >> 8 = 4096 = 2*2048 + 0.
    constexpr std::uint64_t pk = 1u << 20;
    EXPECT_EQ(DirIndexAt(pk, 2, 0), 2u);
    EXPECT_EQ(DirIndexAt(pk, 2, 1), 0u);
}

TEST(WaystoneDirIndexTest, ADepth1WalkUsesTheWholeLogicalIndex) {
    EXPECT_EQ(DirIndexAt(0, 1, 0), 0u);
    EXPECT_EQ(DirIndexAt(256, 1, 0), 1u);
    EXPECT_EQ(DirIndexAt(kDepth1Coverage - 1, 1, 0), kDirFanout - 1);
}

}  // namespace
}  // namespace kds::stats
