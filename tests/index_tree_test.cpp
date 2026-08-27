#include "kds/storage/index/index_tree.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/page_header.hpp"

// The secondary index tree (docs/spec/index.md §4, workplan IX02).
//
// What separates this from btree_test.cpp is the thing the whole page class
// exists for: **keys arrive out of order and a full page divides.** The
// clustered tree refuses that case on purpose; here it is the ordinary path,
// so these tests drive it deliberately - random insertion, narrow pages, and
// enough entries to split every level including the root.
//
// Three properties are load-bearing and are asserted rather than assumed:
//
//   1. **A separator is the low key of the subtree it points at.** An
//      internal split *pushes* its median up and a leaf split *copies* its
//      boundary up; getting either backwards lands a descent one page off
//      and loses rows silently.
//   2. **Duplicate keys stay reachable.** A secondary key is not unique, so
//      duplicates straddle leaf boundaries - the reason the sort key is
//      (key, pk) and the reason a probe is zero-padded rather than
//      shortened.
//   3. **A walk is in key order and complete.** Everything inserted comes
//      back, once, sorted.

namespace kds::index {
namespace {

// A tiny index: a 4-byte key, so a leaf holds 8144 / (4 + 8) = 678 entries.
// Narrow enough that a few thousand inserts split every level.
constexpr std::uint16_t kKeyWidth = 4;

IndexLayout Layout(std::uint16_t key_width = kKeyWidth, std::uint16_t covered = 0) {
    return IndexLayout{key_width, covered};
}

// A key whose bytes order like the integer, which is what the real encoder
// produces (exec/index_key.hpp) and all this layer cares about.
std::vector<std::byte> Key(std::uint32_t v, std::uint16_t width = kKeyWidth) {
    std::vector<std::byte> out(width, std::byte{0});
    for (std::uint16_t i = 0; i < width && i < 4; ++i) {
        out[width - 1 - i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    return out;
}

std::uint32_t KeyOf(std::span<const std::byte> key) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < key.size(); ++i) {
        v = (v << 8) | static_cast<std::uint32_t>(key[i]);
    }
    return v;
}

// A fresh store with the index's root already formatted.
struct Fixture {
    storage::InMemoryPageStore store;
    PageId root = kInvalidPageId;
    IndexLayout layout;

    explicit Fixture(IndexLayout l = Layout()) : layout(l) {
        auto created = store.CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        root = created.value().first;
        Status s = FormatRoot(created.value().second.bytes(), layout, /*owner_oid=*/0);
        EXPECT_TRUE(s.ok()) << s.message();
    }

    Status Insert(std::uint32_t key, std::uint64_t pk,
                  std::span<const std::byte> covered = {}) {
        auto out = IndexInsert(store, root, layout, Key(key, layout.key_width), pk, covered, /*owner_oid=*/0);
        if (!out.ok()) return out.status();
        if (out.value().new_root != kInvalidPageId) root = out.value().new_root;
        return Status::OK();
    }

    // Every (key, pk) in the tree, in walk order.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> Walk() {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
        Status s = IndexVisit(
            store, root, layout, storage::PageAccess::kRead,
            [&](PageId, IndexLeafView& leaf,
                std::uint16_t idx) -> StatusOr<storage::VisitControl> {
                auto entry = leaf.Entry(idx);
                if (!entry.ok()) return entry.status();
                out.emplace_back(KeyOf(entry.value().subspan(0, layout.key_width)),
                                 GetIndexPk(entry.value().subspan(layout.key_width)));
                return storage::VisitControl::kContinue;
            });
        EXPECT_TRUE(s.ok()) << s.message();
        return out;
    }

    // The pks carrying `key`, found the way a probe will: seek to the leaf
    // that could hold it, then walk right while the key still matches.
    std::vector<std::uint64_t> Probe(std::uint32_t key) {
        std::vector<std::byte> sort_key(layout.sort_key_width(), std::byte{0});
        auto k = Key(key, layout.key_width);
        std::memcpy(sort_key.data(), k.data(), k.size());  // pk left zero: the lower bound

        auto leaf = IndexSeekLeaf(store, root, layout, sort_key);
        EXPECT_TRUE(leaf.ok()) << leaf.status().message();

        std::vector<std::uint64_t> out;
        Status s = IndexVisitFrom(
            store, leaf.value(), layout, storage::PageAccess::kRead,
            [&](PageId, IndexLeafView& page,
                std::uint16_t idx) -> StatusOr<storage::VisitControl> {
                auto entry = page.Entry(idx);
                if (!entry.ok()) return entry.status();
                const std::uint32_t found = KeyOf(entry.value().subspan(0, layout.key_width));
                if (found < key) return storage::VisitControl::kContinue;
                if (found > key) return storage::VisitControl::kStop;
                out.push_back(GetIndexPk(entry.value().subspan(layout.key_width)));
                return storage::VisitControl::kContinue;
            });
        EXPECT_TRUE(s.ok()) << s.message();
        return out;
    }
};

// ---- One page -----------------------------------------------------------

TEST(IndexTreeTest, AFreshRootIsOneEmptyLeaf) {
    Fixture f;
    auto height = IndexHeight(f.store, f.root, f.layout);
    ASSERT_TRUE(height.ok()) << height.status().message();
    EXPECT_EQ(1, height.value());

    auto leaves = IndexLeafCount(f.store, f.root, f.layout);
    ASSERT_TRUE(leaves.ok());
    EXPECT_EQ(1u, leaves.value());

    EXPECT_TRUE(f.Walk().empty());
}

TEST(IndexTreeTest, EntriesComeBackInKeyOrderWhateverOrderTheyArrived) {
    Fixture f;
    // Deliberately adversarial for the clustered tree's append-only split:
    // descending, then interleaved.
    for (std::uint32_t k = 50; k >= 1; --k) ASSERT_TRUE(f.Insert(k, k).ok());
    for (std::uint32_t k = 100; k > 50; --k) ASSERT_TRUE(f.Insert(k, k).ok());

    const auto walked = f.Walk();
    ASSERT_EQ(100u, walked.size());
    for (std::size_t i = 0; i < walked.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint32_t>(i + 1), walked[i].first);
    }
}

// ---- Splitting ----------------------------------------------------------

// Inserts `count` keys in shuffled order, then asserts the tree came back
// complete, sorted, and reachable by descent as well as by walk - the two
// being different failures. A walk follows sibling links and would survive a
// separator pointing one page off; a probe would not.
void ExpectCompleteAfterRandomInserts(Fixture& f, std::uint32_t count) {
    std::vector<std::uint32_t> keys(count);
    for (std::uint32_t i = 0; i < count; ++i) keys[i] = i;
    std::shuffle(keys.begin(), keys.end(), std::mt19937(12345));

    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(f.Insert(keys[i], keys[i] + 1).ok()) << "key " << keys[i];
    }

    const auto walked = f.Walk();
    ASSERT_EQ(count, walked.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        // Every pk still attached to its own key, which is what a division
        // that moved an entry to the wrong half would break.
        EXPECT_EQ(i, walked[i].first);
        EXPECT_EQ(i + 1, walked[i].second);
    }

    for (std::uint32_t i = 0; i < count; i += 97) {
        const auto pks = f.Probe(i);
        ASSERT_EQ(1u, pks.size()) << "key " << i;
        EXPECT_EQ(i + 1, pks[0]);
    }
}

TEST(IndexTreeTest, RandomInsertsStayCompleteAcrossManyLeafSplits) {
    // 40,000 entries at 678 per leaf, halved by each split, is ~120 leaves -
    // enough to divide leaves constantly and to fill one internal level.
    Fixture f;
    ExpectCompleteAfterRandomInserts(f, 40000);

    auto height = IndexHeight(f.store, f.root, f.layout);
    ASSERT_TRUE(height.ok());
    EXPECT_GE(height.value(), 2) << "no leaf split; this test is not exercising the path";
}

TEST(IndexTreeTest, ADeepTreeSplitsItsInternalNodesAndItsRoot) {
    // Leaf splits alone never reach the internal-node split or the root
    // growth above it - with a 678-entry fan-out that needs ~170,000 rows.
    // A wide key gets there directly: 1,000 bytes puts 8 entries in a leaf
    // and 8 in a node, so a few thousand inserts propagate splits through
    // every level several times over. The same arithmetic a real index on a
    // wide composite key would have.
    Fixture f(Layout(/*key_width=*/1000));
    ExpectCompleteAfterRandomInserts(f, 3000);

    auto height = IndexHeight(f.store, f.root, f.layout);
    ASSERT_TRUE(height.ok());
    EXPECT_GE(height.value(), 4) << "the root never split; the push-up path is untested";
}

TEST(IndexTreeTest, EveryLeafsKeysSitBelowItsRightSiblings) {
    // The structural invariant a bad split breaks first, checked directly
    // against the sibling links rather than inferred from the walk.
    Fixture f;
    for (std::uint32_t i = 0; i < 5000; ++i) ASSERT_TRUE(f.Insert((i * 7919) % 5000, i + 1).ok());

    std::uint32_t previous_high = 0;
    bool first = true;
    Status s = IndexVisit(f.store, f.root, f.layout, storage::PageAccess::kRead,
                          [&](PageId, IndexLeafView& leaf,
                              std::uint16_t idx) -> StatusOr<storage::VisitControl> {
                              auto entry = leaf.Entry(idx);
                              if (!entry.ok()) return entry.status();
                              const std::uint32_t key =
                                  KeyOf(entry.value().subspan(0, f.layout.key_width));
                              // Braced: EXPECT_LE expands to an if/else, so
                              // an unbraced guard dangles the else onto it.
                              if (!first) {
                                  EXPECT_LE(previous_high, key);
                              }
                              previous_high = key;
                              first = false;
                              return storage::VisitControl::kContinue;
                          });
    ASSERT_TRUE(s.ok()) << s.message();
}

// ---- Duplicates ---------------------------------------------------------

TEST(IndexTreeTest, ADuplicatedKeyStaysReachableAcrossLeafBoundaries) {
    // The case the (key, pk) sort key exists for. 2,000 entries share one
    // key, which is three leaves' worth, so the duplicates certainly
    // straddle boundaries and a separator lands inside the run.
    Fixture f;
    constexpr std::uint64_t kDuplicates = 2000;
    for (std::uint64_t pk = 1; pk <= kDuplicates; ++pk) ASSERT_TRUE(f.Insert(42, pk).ok());
    // Neighbours on both sides, so a probe that over- or under-shoots shows.
    for (std::uint64_t pk = 1; pk <= 500; ++pk) ASSERT_TRUE(f.Insert(41, 100000 + pk).ok());
    for (std::uint64_t pk = 1; pk <= 500; ++pk) ASSERT_TRUE(f.Insert(43, 200000 + pk).ok());

    const auto pks = f.Probe(42);
    ASSERT_EQ(kDuplicates, pks.size());
    for (std::uint64_t i = 0; i < kDuplicates; ++i) EXPECT_EQ(i + 1, pks[i]);
}

TEST(IndexTreeTest, AByteIdenticalEntryIsReportedRatherThanStoredTwice) {
    // Nothing reclaims an index entry, and a probe that resolved one pk
    // twice would emit its row twice.
    Fixture f;
    ASSERT_TRUE(f.Insert(7, 3).ok());

    auto again = IndexInsert(f.store, f.root, f.layout, Key(7), 3, {}, /*owner_oid=*/0);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_TRUE(again.value().already_present);
    EXPECT_FALSE(again.value().restructured());

    EXPECT_EQ(1u, f.Walk().size());
}

TEST(IndexTreeTest, TwoEntriesDifferingOnlyInCoveredBytesAreBothKept) {
    // A covered column that changed is *information*, not noise: the entry
    // carrying the new value is what a later probe filters on.
    Fixture f(Layout(kKeyWidth, /*covered=*/4));
    const std::vector<std::byte> a(4, std::byte{0x11});
    const std::vector<std::byte> b(4, std::byte{0x22});

    ASSERT_TRUE(f.Insert(7, 3, a).ok());
    ASSERT_TRUE(f.Insert(7, 3, b).ok());
    EXPECT_EQ(2u, f.Walk().size());

    // ...and a third that repeats one of them is still deduplicated.
    auto third = IndexInsert(f.store, f.root, f.layout, Key(7), 3, b, /*owner_oid=*/0);
    ASSERT_TRUE(third.ok());
    EXPECT_TRUE(third.value().already_present);
    EXPECT_EQ(2u, f.Walk().size());
}

// ---- Seeks and stopping -------------------------------------------------

TEST(IndexTreeTest, AProbeForAnAbsentKeyReturnsNothingWithoutFailing) {
    Fixture f;
    for (std::uint32_t k = 0; k < 3000; k += 2) ASSERT_TRUE(f.Insert(k, k + 1).ok());
    EXPECT_TRUE(f.Probe(1501).empty());
    // Past the right edge, where the descent runs off the end of the tree.
    EXPECT_TRUE(f.Probe(999999).empty());
}

TEST(IndexTreeTest, AStopEndsTheWalkSuccessfully) {
    Fixture f;
    for (std::uint32_t k = 0; k < 2000; ++k) ASSERT_TRUE(f.Insert(k, k + 1).ok());

    int seen = 0;
    Status s = IndexVisit(f.store, f.root, f.layout, storage::PageAccess::kRead,
                          [&](PageId, IndexLeafView&,
                              std::uint16_t) -> StatusOr<storage::VisitControl> {
                              return ++seen == 5 ? storage::VisitControl::kStop
                                                 : storage::VisitControl::kContinue;
                          });
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(5, seen);
}

TEST(IndexTreeTest, AVisitorReturningAnOkStatusWithNoControlIsADefect) {
    Fixture f;
    ASSERT_TRUE(f.Insert(1, 1).ok());
    Status s = IndexVisit(
        f.store, f.root, f.layout, storage::PageAccess::kRead,
        [](PageId, IndexLeafView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            return Status::OK();
        });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, s.code());
}

// ---- Refusals and damage ------------------------------------------------

TEST(IndexTreeTest, ALayoutNoPageCouldSplitIsRefused) {
    // Where a large COVERING clause is refused: by arithmetic, at
    // declaration, rather than by an insert that fails much later.
    EXPECT_FALSE(CheckIndexLayout(Layout(kKeyWidth, /*covered=*/8000)).ok());
    EXPECT_FALSE(CheckIndexLayout(Layout(/*key_width=*/0)).ok());
    EXPECT_TRUE(CheckIndexLayout(Layout(kKeyWidth, /*covered=*/64)).ok());
}

TEST(IndexTreeTest, AWidthDisagreeingWithTheIndexIsCorruptionNotAReinterpretation) {
    // The checked half of index_page.hpp's redundancy: the page says what it
    // holds, and a page that disagrees with the catalog is damage.
    Fixture f;
    ASSERT_TRUE(f.Insert(1, 1).ok());

    IndexLayout wrong = f.layout;
    wrong.key_width = kKeyWidth + 4;

    std::vector<std::byte> probe(wrong.sort_key_width(), std::byte{0});
    auto seek = IndexSeekLeaf(f.store, f.root, wrong, probe);
    EXPECT_FALSE(seek.ok());
    EXPECT_EQ(StatusCode::kCorruption, seek.status().code());
}

TEST(IndexTreeTest, ACorruptedLeafIsRejectedRatherThanDescended) {
    Fixture f;
    for (std::uint32_t k = 0; k < 100; ++k) ASSERT_TRUE(f.Insert(k, k + 1).ok());

    // Claim more entries than the page can hold. A build that trusted the
    // count would read past the array; this one must say so.
    auto page = f.store.Get(f.root);
    ASSERT_TRUE(page.ok());
    const std::uint16_t absurd = 60000;
    std::memcpy(page.value().bytes().data() + kIndexHeaderOffset + kIndexLeafNrEntriesOffset, &absurd,
                sizeof(absurd));

    std::vector<std::byte> probe(f.layout.sort_key_width(), std::byte{0});
    auto seek = IndexSeekLeaf(f.store, f.root, f.layout, probe);
    EXPECT_FALSE(seek.ok());
    EXPECT_EQ(StatusCode::kCorruption, seek.status().code());
}

TEST(IndexTreeTest, TheClusteredTreesPagesAreNotThisTrees) {
    // An index tree handed a clustered-tree root must say so rather than
    // parse a heap page's slot directory as an entry array.
    Fixture f;
    auto page = f.store.Get(f.root);
    ASSERT_TRUE(page.ok());
    storage::FormatPage(page.value().bytes(), PageType::kBtreeLeaf);

    std::vector<std::byte> probe(f.layout.sort_key_width(), std::byte{0});
    auto seek = IndexSeekLeaf(f.store, f.root, f.layout, probe);
    EXPECT_FALSE(seek.ok());
    EXPECT_EQ(StatusCode::kCorruption, seek.status().code());
}

// ---- What the WAL will be told ------------------------------------------

TEST(IndexTreeTest, AnOrdinaryInsertReportsNoStructureAndASplitReportsBoth) {
    // `changes()` names pages **no record type describes**, so a plain
    // append reports none: the entry bytes describe it completely and the
    // caller logs one small INDEX_INSERT instead of an 8 KB page image
    // (wal/record.hpp). This asserted the opposite when IX02 landed, before
    // there was a record type to describe an entry.
    Fixture f;
    auto first = IndexInsert(f.store, f.root, f.layout, Key(1), 1, {}, /*owner_oid=*/0);
    ASSERT_TRUE(first.ok());
    EXPECT_FALSE(first.value().restructured());
    EXPECT_TRUE(first.value().changes().empty());

    // Fill the root leaf, then one more.
    const std::uint16_t per_leaf = MaxLeafEntries(f.layout);
    for (std::uint32_t k = 2; k <= per_leaf; ++k) {
        ASSERT_TRUE(f.Insert(k, k).ok()) << "key " << k;
    }
    auto split = IndexInsert(f.store, f.root, f.layout, Key(per_leaf + 1), per_leaf + 1, {}, /*owner_oid=*/0);
    ASSERT_TRUE(split.ok()) << split.status().message();
    EXPECT_TRUE(split.value().restructured());
    EXPECT_NE(kInvalidPageId, split.value().new_root) << "the first split must grow a root";

    // The new leaf, the old one, and the new root - each named exactly once,
    // with the new pages distinguishable from the rewritten ones.
    int new_pages = 0;
    for (const IndexChange& c : split.value().changes()) {
        if (c.is_new_page) ++new_pages;
    }
    EXPECT_EQ(2, new_pages) << "the sibling and the root are new; the split page is rewritten";
}

}  // namespace
}  // namespace kds::index
