#include "kds/stats/waystone_dir.hpp"

#include <array>
#include <cstring>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "kds/stats/waystone.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// The per-pattern waystone directory (docs/spec/waystone-concpets.md §5). What
// is pinned here beyond the walk itself: that an unpopulated range is a
// *miss* rather than an error, that allocation is sparse, that a collision
// resolves through the waystone header and never into a foreign trail, and
// what growth actually preserves - which for a hash key is not everything,
// and the tests say so rather than avoiding the case.

namespace kds::stats {
namespace {

using storage::InMemoryPageStore;

constexpr std::uint64_t kPatternId = 0xA1A2A3A4B1B2B3B4ull;

// One pattern's directory, so every walk below is for one pattern and only
// the `arg_hash` varies - which is what a directory is: the second level of
// addressing, under a pattern_id the caller already resolved.
//
// The walk consumes only `arg_hash`; the pattern travels with it because
// the page id it returns is not usable without one (instance_key.hpp).
constexpr InstanceKey Key(std::uint64_t arg_hash) noexcept {
    return InstanceKey{kPatternId, arg_hash};
}

// The digit an independent formulation says a walk should take: base-2048
// numeral, most significant digit first. Division and modulo rather than
// the implementation's shift and mask, so the two can disagree.
std::size_t ExpectedDigit(std::uint64_t key, int depth, int level) {
    std::uint64_t divisor = 1;
    for (int i = 0; i < depth - 1 - level; ++i) divisor *= kDirFanout;
    return static_cast<std::size_t>((key / divisor) % kDirFanout);
}

PageId ChildOf(storage::PageStore& store, PageId dir_page, std::size_t index) {
    auto bytes = store.Get(dir_page);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message();
    PageId child = kEmptyDirSlot;
    std::memcpy(&child, bytes.value().bytes().data() + index * sizeof(PageId), sizeof(PageId));
    return child;
}

// Formats the page a lookup resolved to as the trail of `arg_hash`, which
// is what P08 will do and what makes the header check testable here.
void FormatAs(storage::PageStore& store, PageId page_id, std::uint64_t arg_hash) {
    auto bytes = store.Get(page_id);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    FormatWaystonePage(bytes.value().bytes(), {kPatternId, arg_hash}, /*recorded_ts=*/7);
}

bool PageHolds(storage::PageStore& store, PageId page_id, std::uint64_t arg_hash) {
    auto bytes = store.Get(page_id);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message();
    return WaystonePageHolds(bytes.value().bytes(), {kPatternId, arg_hash});
}

// ---- Derived constants ----------------------------------------------------

TEST(WaystoneDirLayoutTest, FanoutAndDepthAreDerivedFromThePageAndTheKey) {
    EXPECT_EQ(kDirFanout, 2048u);
    EXPECT_EQ(kDirFanout * sizeof(PageId), kPageSize);
    EXPECT_EQ(kDirIndexMask, 2047u);

    // ceil(64 / 11) = 6: enough levels to address every bit of the hash,
    // and one fewer would leave the top bits unconsumed.
    EXPECT_EQ(kMaxPatternDirDepth, 6);
    EXPECT_GE(kMaxPatternDirDepth * kDirFanoutBits, 64);
    EXPECT_LT((kMaxPatternDirDepth - 1) * kDirFanoutBits, 64);

    // Never 0: page 0 is the superblock, so an all-zero interior page must
    // not read as 2048 links to it.
    EXPECT_EQ(kEmptyDirSlot, kInvalidPageId);
}

// ---- The walk -------------------------------------------------------------

TEST(WaystoneDirIndexTest, DigitsAreBase2048MostSignificantFirst) {
    constexpr std::uint64_t kKey = 0x0123456789ABCDEFull;

    EXPECT_EQ(DirIndexAt(kKey, 1, 0), 0x5EFu);  // low 11 bits

    for (int depth = 1; depth <= kMaxPatternDirDepth; ++depth) {
        for (int level = 0; level < depth; ++level) {
            EXPECT_EQ(DirIndexAt(kKey, depth, level), ExpectedDigit(kKey, depth, level))
                << "depth=" << depth << " level=" << level;
        }
    }

    // Ordering stated directly: a two-digit key resolves through its high
    // digit at the root and its low digit below it.
    const std::uint64_t composed = (17ull << kDirFanoutBits) | 300ull;
    EXPECT_EQ(DirIndexAt(composed, 2, 0), 17u);
    EXPECT_EQ(DirIndexAt(composed, 2, 1), 300u);
}

TEST(WaystoneDirIndexTest, BitsAboveTheDepthFoldRatherThanFail) {
    // The pk directory refused a key past its coverage; a hash has no
    // coverage to exceed, so the high bits are simply not consumed and two
    // keys agreeing on the low ones share an address. That is the
    // collision the waystone header exists to catch.
    const std::uint64_t a = 0x1234ull;
    const std::uint64_t b = a | (1ull << 40);
    EXPECT_EQ(DirIndexAt(a, 1, 0), DirIndexAt(b, 1, 0));

    // At the maximum depth the top digit's two high bits are always zero,
    // which is why a seventh level would buy nothing.
    EXPECT_EQ(DirIndexAt(~0ull, kMaxPatternDirDepth, 0), (1u << 9) - 1);
}

// ---- Lookup ---------------------------------------------------------------

class WaystoneDirTest : public ::testing::Test {
protected:
    InMemoryPageStore store_{128};

    PageId MakeRoot() {
        auto root = CreateDirPage(store_);
        EXPECT_TRUE(root.ok()) << root.status().message();
        return root.ok() ? root.value() : kInvalidPageId;
    }
};

TEST_F(WaystoneDirTest, AFreshRootIsEmptyAtEverySlot) {
    const PageId root = MakeRoot();
    for (std::size_t i = 0; i < kDirFanout; ++i) {
        ASSERT_EQ(ChildOf(store_, root, i), kEmptyDirSlot) << "slot " << i;
    }
}

TEST_F(WaystoneDirTest, AnUnpopulatedRangeIsAMissNotAnError) {
    const PageId root = MakeRoot();
    for (int depth = 1; depth <= kMaxPatternDirDepth; ++depth) {
        auto found = LookupWaystonePage(store_, root, depth, Key(0xDEADBEEFCAFEull));
        ASSERT_TRUE(found.ok()) << found.status().message();
        EXPECT_EQ(found.value(), kInvalidPageId) << "depth " << depth;
    }
}

TEST_F(WaystoneDirTest, CreateThenLookupResolvesToTheSamePageAtEveryDepth) {
    constexpr std::uint64_t kArgHash = 0x0F1E2D3C4B5A6978ull;

    for (int depth = 1; depth <= kMaxPatternDirDepth; ++depth) {
        InMemoryPageStore store{128};
        auto root = CreateDirPage(store);
        ASSERT_TRUE(root.ok()) << root.status().message();

        auto created = LookupOrCreateWaystonePage(store, root.value(), depth, Key(kArgHash));
        ASSERT_TRUE(created.ok()) << created.status().message();
        ASSERT_NE(created.value(), kInvalidPageId);

        auto found = LookupWaystonePage(store, root.value(), depth, Key(kArgHash));
        ASSERT_TRUE(found.ok()) << found.status().message();
        EXPECT_EQ(found.value(), created.value()) << "depth " << depth;
    }
}

TEST_F(WaystoneDirTest, CreatingTwiceReturnsTheSamePageAndAllocatesNothing) {
    const PageId root = MakeRoot();
    constexpr std::uint64_t kArgHash = 0x00A0B0C0D0E0F001ull;

    auto first = LookupOrCreateWaystonePage(store_, root, 3, Key(kArgHash));
    ASSERT_TRUE(first.ok()) << first.status().message();
    const std::size_t after_first = store_.page_count();

    auto second = LookupOrCreateWaystonePage(store_, root, 3, Key(kArgHash));
    ASSERT_TRUE(second.ok()) << second.status().message();

    EXPECT_EQ(second.value(), first.value());
    EXPECT_EQ(store_.page_count(), after_first);
}

TEST_F(WaystoneDirTest, DistinctInstancesGetDistinctPages) {
    const PageId root = MakeRoot();

    std::set<PageId> pages;
    for (std::uint64_t i = 0; i < 16; ++i) {
        auto created = LookupOrCreateWaystonePage(store_, root, 2, Key((i << 20) | (i * 37 + 1)));
        ASSERT_TRUE(created.ok()) << created.status().message();
        pages.insert(created.value());
    }
    EXPECT_EQ(pages.size(), 16u);
}

TEST_F(WaystoneDirTest, AllocationIsSparseAndCostsOnlyThePathItTouches) {
    const PageId root = MakeRoot();
    ASSERT_EQ(store_.page_count(), 1u);  // the root

    // Depth 3 over a 2048^3-slot space: one instance costs two interior
    // pages plus the waystone itself, not the 2048^2 pages a dense
    // directory would.
    auto first = LookupOrCreateWaystonePage(store_, root, 3, Key(0));
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(store_.page_count(), 4u);

    // A key differing only in the last digit reuses both interior pages.
    auto sibling = LookupOrCreateWaystonePage(store_, root, 3, Key(1));
    ASSERT_TRUE(sibling.ok()) << sibling.status().message();
    EXPECT_EQ(store_.page_count(), 5u);

    // A key differing in the top digit shares nothing below the root.
    auto far = LookupOrCreateWaystonePage(store_, root, 3, Key(1ull << 22));
    ASSERT_TRUE(far.ok()) << far.status().message();
    EXPECT_EQ(store_.page_count(), 8u);
}

TEST_F(WaystoneDirTest, ANewlyCreatedTargetIsUnformattedAndReadsAsAMiss) {
    const PageId root = MakeRoot();
    constexpr std::uint64_t kArgHash = 0x5151515151515151ull;

    auto created = LookupOrCreateWaystonePage(store_, root, 2, Key(kArgHash));
    ASSERT_TRUE(created.ok()) << created.status().message();

    // The directory resolves an address; it does not record a trail. Until
    // the writer formats the page it holds nothing, and a reader treats
    // that exactly as it treats an empty slot.
    EXPECT_FALSE(PageHolds(store_, created.value(), kArgHash));

    FormatAs(store_, created.value(), kArgHash);
    EXPECT_TRUE(PageHolds(store_, created.value(), kArgHash));
}

// ---- Collisions -----------------------------------------------------------

TEST_F(WaystoneDirTest, ACollidingInstanceResolvesToAMissNeverToAForeignTrail) {
    const PageId root = MakeRoot();

    // Two hashes agreeing on the low 11 bits: at depth 1 they address the
    // same slot, which is the whole hazard of keying a directory by a
    // hash.
    constexpr std::uint64_t kRecorded = (99ull << kDirFanoutBits) | 512ull;
    constexpr std::uint64_t kColliding = (44ull << kDirFanoutBits) | 512ull;
    ASSERT_EQ(DirIndexAt(kRecorded, 1, 0), DirIndexAt(kColliding, 1, 0));

    auto created = LookupOrCreateWaystonePage(store_, root, 1, Key(kRecorded));
    ASSERT_TRUE(created.ok()) << created.status().message();
    FormatAs(store_, created.value(), kRecorded);

    // The walk hands the colliding instance the same address - it has no
    // way not to - and the waystone's own header is what stops that from
    // becoming somebody else's rows.
    auto found = LookupWaystonePage(store_, root, 1, Key(kColliding));
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), created.value());
    EXPECT_FALSE(PageHolds(store_, found.value(), kColliding));
    EXPECT_TRUE(PageHolds(store_, found.value(), kRecorded));
}

TEST_F(WaystoneDirTest, APatternIdMismatchIsAMissToo) {
    const PageId root = MakeRoot();
    constexpr std::uint64_t kArgHash = 0x2222333344445555ull;

    auto created = LookupOrCreateWaystonePage(store_, root, 2, Key(kArgHash));
    ASSERT_TRUE(created.ok()) << created.status().message();
    FormatAs(store_, created.value(), kArgHash);

    auto bytes = store_.Get(created.value());
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_FALSE(WaystonePageHolds(bytes.value().bytes(), {kPatternId + 1, kArgHash}));
}

// ---- Growth ---------------------------------------------------------------

TEST_F(WaystoneDirTest, GrowthRelinksTheOldRootUnderSlotZero) {
    const PageId root = MakeRoot();

    auto grown = GrowPatternDirectory(store_, root, 1);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    EXPECT_NE(grown.value(), root);
    EXPECT_EQ(ChildOf(store_, grown.value(), 0), root);
    EXPECT_EQ(ChildOf(store_, grown.value(), 1), kEmptyDirSlot);

    // O(1): one new page, nothing rewritten.
    EXPECT_EQ(store_.page_count(), 2u);
}

TEST_F(WaystoneDirTest, GrowthPreservesTheMappingsWhoseNewTopDigitIsZero) {
    const PageId root = MakeRoot();

    // At depth 1 this key's whole address is its low digit; its bits
    // [11, 22) are zero, so the new root reaches it through slot 0.
    constexpr std::uint64_t kSurvives = 300ull;
    ASSERT_EQ(DirIndexAt(kSurvives, 2, 0), 0u);

    auto created = LookupOrCreateWaystonePage(store_, root, 1, Key(kSurvives));
    ASSERT_TRUE(created.ok()) << created.status().message();
    FormatAs(store_, created.value(), kSurvives);

    auto grown = GrowPatternDirectory(store_, root, 1);
    ASSERT_TRUE(grown.ok()) << grown.status().message();

    auto found = LookupWaystonePage(store_, grown.value(), 2, Key(kSurvives));
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), created.value());
    EXPECT_TRUE(PageHolds(store_, found.value(), kSurvives));
}

TEST_F(WaystoneDirTest, GrowthCoolsEveryOtherInstanceWithoutCorruptingOne) {
    const PageId root = MakeRoot();

    // The honest half of the previous test. A hash has no reason to carry
    // zeros in the digit a new level consumes, so 2047 of every 2048
    // instances become unreachable at their old address when the directory
    // deepens. That is a cache flush, not a correctness event (invariant
    // 8), and the next execution re-records the trail.
    constexpr std::uint64_t kCooled = (5ull << kDirFanoutBits) | 300ull;
    ASSERT_NE(DirIndexAt(kCooled, 2, 0), 0u);

    auto created = LookupOrCreateWaystonePage(store_, root, 1, Key(kCooled));
    ASSERT_TRUE(created.ok()) << created.status().message();
    FormatAs(store_, created.value(), kCooled);

    auto grown = GrowPatternDirectory(store_, root, 1);
    ASSERT_TRUE(grown.ok()) << grown.status().message();

    auto found = LookupWaystonePage(store_, grown.value(), 2, Key(kCooled));
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), kInvalidPageId);

    // Not leaked, either: the old subtree still hangs off slot 0, so the
    // page is reachable by whichever key now addresses it - and that key
    // gets a header mismatch, the same miss a collision gets.
    constexpr std::uint64_t kNowAddressesIt = 300ull;
    auto other = LookupWaystonePage(store_, grown.value(), 2, Key(kNowAddressesIt));
    ASSERT_TRUE(other.ok()) << other.status().message();
    EXPECT_EQ(other.value(), created.value());
    EXPECT_FALSE(PageHolds(store_, other.value(), kNowAddressesIt));
}

TEST_F(WaystoneDirTest, GrowthStopsAtTheDepthThatAddressesTheWholeKey) {
    const PageId root = MakeRoot();

    auto refused = GrowPatternDirectory(store_, root, kMaxPatternDirDepth);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kOutOfRange);
}

// ---- Depth validation -----------------------------------------------------

TEST_F(WaystoneDirTest, ADepthOutsideTheLegalRangeIsRefusedByEveryEntryPoint) {
    const PageId root = MakeRoot();

    for (int depth : {0, -1, kMaxPatternDirDepth + 1}) {
        auto looked = LookupWaystonePage(store_, root, depth, Key(1));
        EXPECT_FALSE(looked.ok()) << "depth " << depth;
        EXPECT_EQ(looked.status().code(), StatusCode::kInvalidArgument);

        auto created = LookupOrCreateWaystonePage(store_, root, depth, Key(1));
        EXPECT_FALSE(created.ok()) << "depth " << depth;
        EXPECT_EQ(created.status().code(), StatusCode::kInvalidArgument);

        auto grown = GrowPatternDirectory(store_, root, depth);
        EXPECT_FALSE(grown.ok()) << "depth " << depth;
        EXPECT_EQ(grown.status().code(), StatusCode::kInvalidArgument);
    }
}

TEST_F(WaystoneDirTest, ADanglingChildIdIsReportedRatherThanFollowed) {
    const PageId root = MakeRoot();

    // Damage in an interior page - which carries no checksum to catch it -
    // surfaces as the store's own NotFound rather than as a walk into a
    // page that was never created.
    auto bytes = store_.Get(root);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    const PageId bogus = 9999;
    std::memcpy(bytes.value().bytes().data() + DirIndexAt(7, 2, 0) * sizeof(PageId), &bogus,
                sizeof(PageId));

    auto found = LookupWaystonePage(store_, root, 2, Key(7));
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);
}

// ---- Headerless interior pages -------------------------------------------

TEST(WaystoneDirDeviceTest, InteriorPagesAreHeaderlessAndSurviveAFlushIntact) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/16, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();

    auto opened = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto& store = *opened.value();

    auto root = CreateDirPage(store);
    ASSERT_TRUE(root.ok()) << root.status().message();

    // Child 1 lives at byte offset 4 - exactly where DevicePageStore
    // stamps a checksum into a headered frame. The whole reason these
    // pages are allocated headerless is that the stamp would eat it.
    constexpr std::uint64_t kArgHash = 1;
    ASSERT_EQ(DirIndexAt(kArgHash, 1, 0), 1u);
    auto created = LookupOrCreateWaystonePage(store, root.value(), 1, Key(kArgHash));
    ASSERT_TRUE(created.ok()) << created.status().message();

    EXPECT_TRUE(store.IsHeaderless(root.value()));
    // The waystone itself is headered - it is not addressed by arithmetic,
    // so it keeps its checksum and page_lsn (spec §6).
    EXPECT_FALSE(store.IsHeaderless(created.value()));

    ASSERT_TRUE(store.Flush().ok());

    // Reopened from the device, which re-reads and re-verifies every page
    // it is asked for: a checksum stamped over child 1 would show up here
    // as either Corruption or a mangled link.
    auto reopened = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();

    auto found = LookupWaystonePage(*reopened.value(), root.value(), 1, Key(kArgHash));
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), created.value());
}

}  // namespace
}  // namespace kds::stats
