#include "kds/storage/btree/btree.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

#include "act_on_fetch_store.hpp"
#include "device_room.hpp"
#include "tree_structure.hpp"

// The clustered B+ tree: descent, the split that moves nothing, the level
// growth above it, and the point lookup the whole structure exists for.
//
// Two properties are load-bearing throughout and are asserted directly
// rather than assumed:
//
//   1. **A separator is the low key of the subtree it points at** - the
//      same number as that leaf's min_key, never a separately derived
//      boundary (btree_page.hpp's routing rule). If the two can drift, a
//      descent lands one page off and a lookup silently misses.
//   2. **Nothing here moves a tuple**, so every leaf's min_key stays
//      immutable across splits (invariant 2) and each leaf's ids sit
//      entirely below its right sibling's min_key - the same ordering the
//      heap chain rests on.
//
// The lookup tests care about one thing above all: BtreeLookup is
// **authoritative**. A miss means the row does not exist, so a false
// NotFound is a wrong answer to a query, not a lost optimization.

namespace kds::btree {
namespace {

// A tuple payload: the Keystone word carrying `id`, then `filler` bytes of
// body. The filler size is how these tests control tuples-per-leaf, which
// is how they reach the split paths without inserting millions of rows.
std::vector<std::byte> MakeTuple(std::uint64_t id, std::size_t filler) {
    auto word = Keystone::Encode(id, 0, 0);
    EXPECT_TRUE(word.ok()) << word.status().message();

    std::vector<std::byte> out(kKeystoneWordSize + filler, std::byte{0xAB});
    std::uint64_t v = word.value();
    for (std::size_t i = 0; i < kKeystoneWordSize; ++i) {
        out[i] = static_cast<std::byte>(v & 0xFF);
        v >>= 8;
    }
    return out;
}

std::uint64_t IdOf(std::span<const std::byte> payload) {
    auto id = KeystoneIdOfPayload(payload);
    EXPECT_TRUE(id.ok()) << id.status().message();
    return id.ok() ? id.value() : 0;
}

// Payload sizes chosen against the leaf's usable body: kPageSize (8192)
// minus the slot directory's start (48: 32-byte common header + 16-byte
// heap header) minus the 4-byte next_page_id tail reservation = 8140
// bytes for slots + tuple data. A tuple costs payload + 20 (MVCC header)
// + 5 (slot).
inline constexpr std::size_t kLeafUsableBytes =
    kPageSize - heap::kHeapHeaderOffset - heap::kHeaderSize - sizeof(PageId);
static_assert(kLeafUsableBytes == 8140);

// 5008-byte payload -> 5033 bytes consumed, so exactly one tuple per leaf
// and every insert after the first splits. That is what makes a root
// split reachable in 680 inserts instead of ~4,700.
inline constexpr std::size_t kOnePerLeafFiller = 5000;
static_assert(kKeystoneWordSize + kOnePerLeafFiller + heap::kTupleHeaderOnDiskSize +
                  heap::kSlotOnDiskSize >
              kLeafUsableBytes / 2);

// ~1 KB payloads: a handful per leaf, so a test can watch a leaf fill.
inline constexpr std::size_t kSmallFiller = 56;

// A relation's tree, rooted the way Catalog::CreateTable roots one, with
// the root repointing an insert reports handled the way the statement
// layer has to handle it (InsertPlacement::new_root).
struct Tree {
    explicit Tree(storage::PageStore& store_in) : store(store_in) {
        auto created = store.CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        auto& [page_id, bytes_ref] = created.value();
        const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();
        Status s = FormatRoot(bytes, /*owner_oid=*/0);
        EXPECT_TRUE(s.ok()) << s.message();
        root = page_id;
    }

    StatusOr<storage::InsertPlacement> Insert(std::uint64_t id, std::size_t filler) {
        auto r = BtreeInsert(store, root, id, MakeTuple(id, filler), /*trx_id=*/1, /*owner_oid=*/0);
        if (r.ok() && r.value().new_root != kInvalidPageId) root = r.value().new_root;
        return r;
    }

    // Inserts 1..n, asserting each one lands.
    void Fill(std::uint64_t n, std::size_t filler) {
        for (std::uint64_t id = 1; id <= n; ++id) {
            auto r = Insert(id, filler);
            ASSERT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        }
    }

    storage::PageStore& store;
    PageId root = kInvalidPageId;
};

// Every id the tree holds, in the order a left-to-right leaf walk yields
// them, paired with the leaf it came from.
struct ScannedRow {
    PageId page_id;
    std::uint64_t id;
};

std::vector<ScannedRow> ScanAll(storage::PageStore& store, PageId root) {
    std::vector<ScannedRow> rows;
    Status s = BtreeVisit(
        store, root, storage::PageAccess::kRead,
        [&](PageId page_id, heap::PageView& leaf,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = leaf.ReadTuple(slot);
            if (!tuple.ok()) return storage::VisitControl::kContinue;  // retired slot
            rows.push_back({page_id, IdOf(tuple.value().payload)});
            return storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    return rows;
}

std::uint64_t MinKeyOf(storage::PageStore& store, PageId page_id) {
    auto bytes = store.Get(page_id);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message();
    return heap::PageView(bytes.value().bytes()).min_key();
}

// ---- Shape of a fresh tree ---------------------------------------------

TEST(BtreeTest, EveryPageASplitCreatesCarriesTheOwnerOid) {
    // page.md section 2a: the new leaf, the rebuilt old leaf and the new
    // internal root all carry the relation's oid after the first split.
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto& [root_id, root_bytes_ref] = created.value();
    ASSERT_TRUE(FormatRoot(root_bytes_ref.bytes(), /*owner_oid=*/4001).ok());
    PageId root = root_id;

    storage::InsertPlacement split{};
    bool grew = false;
    for (std::uint64_t id = 1; id <= 200 && !grew; ++id) {
        auto r = BtreeInsert(store, root, id, MakeTuple(id, 1016), /*trx_id=*/1,
                             /*owner_oid=*/4001);
        ASSERT_TRUE(r.ok()) << r.status().message();
        if (r.value().new_root != kInvalidPageId) {
            root = r.value().new_root;
            split = std::move(r.value());
            grew = true;
        }
    }
    ASSERT_TRUE(grew) << "the tree never grew a level";

    for (const PageId changed : split.changes()) {
        auto bytes = store.Get(changed);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()), 4001u) << "page " << changed;
    }
    auto root_again = store.Get(root);
    ASSERT_TRUE(root_again.ok()) << root_again.status().message();
    EXPECT_EQ(storage::GetOwnerOid(root_again.value().bytes()), 4001u);
}

TEST(BtreeTest, AFreshRootIsASingleEmptyLeaf) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    auto bytes = store.Get(tree.root);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_EQ(storage::RawPageType(bytes.value().bytes()),
              static_cast<std::uint8_t>(PageType::kBtreeLeaf));

    heap::PageView leaf(bytes.value().bytes());
    EXPECT_EQ(leaf.min_key(), 0u) << "a relation's first leaf must accept every id";
    EXPECT_EQ(leaf.slot_count(), 0u);
    EXPECT_EQ(leaf.next_page_id(), kInvalidPageId);

    auto height = BtreeHeight(store, tree.root);
    ASSERT_TRUE(height.ok()) << height.status().message();
    EXPECT_EQ(height.value(), 1u) << "no internal level until the root leaf splits";

    auto leaves = BtreeLeafCount(store, tree.root);
    ASSERT_TRUE(leaves.ok()) << leaves.status().message();
    EXPECT_EQ(leaves.value(), 1u);
}

TEST(BtreeTest, InsertsStayOnTheRootLeafUntilItFills) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(10, kSmallFiller);

    // 64-byte tuples: over a hundred fit in one leaf, so ten cannot split
    // it, and nothing structural should have been reported.
    auto height = BtreeHeight(store, tree.root);
    ASSERT_TRUE(height.ok()) << height.status().message();
    EXPECT_EQ(height.value(), 1u);
    EXPECT_EQ(store.page_count(), 1u) << "an unsplit tree must not allocate";

    auto rows = ScanAll(store, tree.root);
    ASSERT_EQ(rows.size(), 10u);
    for (const auto& row : rows) EXPECT_EQ(row.page_id, tree.root);
}

TEST(BtreeTest, AnInsertThatDoesNotSplitReportsNoStructuralChange) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    auto placed = tree.Insert(1, kSmallFiller);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    EXPECT_FALSE(placed.value().restructured());
    EXPECT_TRUE(placed.value().changes().empty());
    EXPECT_EQ(placed.value().new_root, kInvalidPageId);
    EXPECT_EQ(placed.value().page_id, tree.root);
}

// ---- The split that moves nothing ---------------------------------------

TEST(BtreeTest, AFullLeafSplitsRightAndTheNewLeafsMinKeyIsTheIdThatCausedIt) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    const PageId first_leaf = tree.root;

    auto first = tree.Insert(1, kOnePerLeafFiller);
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_FALSE(first.value().restructured()) << "the first tuple fits";

    auto split = tree.Insert(2, kOnePerLeafFiller);
    ASSERT_TRUE(split.ok()) << split.status().message();
    ASSERT_TRUE(split.value().restructured());

    const PageId new_leaf = split.value().page_id;
    EXPECT_NE(new_leaf, first_leaf);
    EXPECT_EQ(MinKeyOf(store, new_leaf), 2u) << "the splitting id is the new leaf's low key";

    // Invariant 2: the split rewrote no existing page's min_key.
    EXPECT_EQ(MinKeyOf(store, first_leaf), 0u);

    // The sibling link is what makes the new leaf reachable to a scan.
    auto old_bytes = store.Get(first_leaf);
    ASSERT_TRUE(old_bytes.ok()) << old_bytes.status().message();
    EXPECT_EQ(heap::PageView(old_bytes.value().bytes()).next_page_id(), new_leaf);

    // Nothing moved: the old leaf still holds exactly the tuple it held.
    EXPECT_EQ(heap::PageView(old_bytes.value().bytes()).slot_count(), 1u);
}

TEST(BtreeTest, ASplitReportsTheNewLeafAndTheRelinkedOldOneToRedo) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    const PageId first_leaf = tree.root;

    ASSERT_TRUE(tree.Insert(1, kOnePerLeafFiller).ok());
    auto split = tree.Insert(2, kOnePerLeafFiller);
    ASSERT_TRUE(split.ok()) << split.status().message();

    // A page mutated with no record describing it is the exact hole the WAL
    // exists to close, so the *contents* of changes() are the contract, not
    // just its non-emptiness.
    const auto changes = split.value().changes();
    ASSERT_GE(changes.size(), 2u);

    // The new leaf first, holding the row. Not a PAGE_INIT the row's insert
    // fills any more: every page a split writes is an image in its one
    // BTREE_SPLIT record, which carries the row (BD-R12).
    EXPECT_EQ(changes[0], split.value().page_id);
    {
        auto new_leaf = store.GetForRead(changes[0]);
        ASSERT_TRUE(new_leaf.ok()) << new_leaf.status().message();
        EXPECT_EQ(heap::PageView(new_leaf.value().bytes()).min_key(), 2u);
    }

    // The old leaf is reported too, whose forward link now reaches it - no
    // record type describes a link edit, so it is an image in the split's
    // record. **Asserted by membership rather than by position**: the
    // link is written after the separator is promoted (H9), so the old leaf
    // now trails the ancestors instead of preceding them. Which page sits at
    // which index was never the contract - the images are self-contained, so
    // redo reconstructs each page whichever order it meets them in. What is
    // the contract is that no mutated page goes unrecorded.
    EXPECT_EQ(std::count(changes.begin(), changes.end(), first_leaf), 1)
        << "the relinked old leaf must be reported exactly once";
}

TEST(BtreeTest, TheFirstSplitGrowsTheTreeToTwoLevelsAndMovesTheRoot) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    const PageId old_root = tree.root;

    ASSERT_TRUE(tree.Insert(1, kOnePerLeafFiller).ok());
    auto split = tree.Insert(2, kOnePerLeafFiller);
    ASSERT_TRUE(split.ok()) << split.status().message();

    ASSERT_NE(split.value().new_root, kInvalidPageId)
        << "splitting a leaf-root has to publish a new root";
    EXPECT_NE(split.value().new_root, old_root);
    EXPECT_EQ(tree.root, split.value().new_root);

    auto root_bytes = store.Get(tree.root);
    ASSERT_TRUE(root_bytes.ok()) << root_bytes.status().message();
    EXPECT_EQ(storage::RawPageType(root_bytes.value().bytes()),
              static_cast<std::uint8_t>(PageType::kBtreeInternal));

    InternalView root(root_bytes.value().bytes());
    EXPECT_EQ(root.level(), 1u) << "one level above the leaves";
    EXPECT_EQ(root.leftmost_child(), old_root) << "the old root keeps its keys";
    ASSERT_EQ(root.entry_count(), 1u);

    auto entry = root.Entry(0);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_EQ(entry.value().sep_key, 2u);
    EXPECT_EQ(entry.value().child, split.value().page_id);

    auto height = BtreeHeight(store, tree.root);
    ASSERT_TRUE(height.ok()) << height.status().message();
    EXPECT_EQ(height.value(), 2u);
}

TEST(BtreeTest, EverySeparatorIsExactlyItsChildLeafsMinKey) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    auto root_bytes = store.Get(tree.root);
    ASSERT_TRUE(root_bytes.ok()) << root_bytes.status().message();
    InternalView root(root_bytes.value().bytes());
    ASSERT_GT(root.entry_count(), 1u) << "test needs a branching root to mean anything";

    // The routing rule: a separator is the low key of the subtree it points
    // at, which is the same number the child page already stores as its
    // min_key. Two differently derived boundaries is how a descent lands
    // one page off; this asserts there is only one.
    EXPECT_EQ(MinKeyOf(store, root.leftmost_child()), 0u);

    std::uint64_t previous = 0;
    for (std::uint16_t i = 0; i < root.entry_count(); ++i) {
        auto entry = root.Entry(i);
        ASSERT_TRUE(entry.ok()) << entry.status().message();
        EXPECT_EQ(entry.value().sep_key, MinKeyOf(store, entry.value().child))
            << "separator " << i;
        EXPECT_GT(entry.value().sep_key, previous) << "entries must stay sorted, entry " << i;
        previous = entry.value().sep_key;
    }
}

TEST(BtreeTest, EachLeafsIdsSitBelowItsRightSiblingsMinKey) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    // The ordering property in full, walked through the sibling links: it
    // is what makes the descent's "this leaf is the only page that may hold
    // this id" claim true, and therefore what makes a lookup miss mean the
    // row does not exist.
    std::vector<PageId> pages;
    std::vector<std::vector<std::uint64_t>> ids_per_page;
    for (const auto& row : ScanAll(store, tree.root)) {
        if (pages.empty() || pages.back() != row.page_id) {
            pages.push_back(row.page_id);
            ids_per_page.emplace_back();
        }
        ids_per_page.back().push_back(row.id);
    }
    ASSERT_GT(pages.size(), 1u) << "test needs a multi-leaf tree";

    for (std::size_t p = 0; p < pages.size(); ++p) {
        const std::uint64_t min_key = MinKeyOf(store, pages[p]);
        for (std::uint64_t id : ids_per_page[p]) {
            EXPECT_GE(id, min_key) << "page " << pages[p];  // invariant 3
            if (p + 1 < pages.size()) {
                EXPECT_LT(id, MinKeyOf(store, pages[p + 1])) << "page " << pages[p];
            }
        }
    }
}

TEST(BtreeTest, AFullRootInternalNodeSplitsAndGrowsAThirdLevel) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // One tuple per leaf, so insert N produces leaf N and separator N. The
    // root absorbs kInternalMaxEntries separators; the one after that has
    // to split it and grow a level. This is the only path that exercises
    // the internal-node split, and it is unreachable at any smaller size.
    const std::uint64_t n = kInternalMaxEntries + 2;  // 680
    tree.Fill(n, kOnePerLeafFiller);

    auto height = BtreeHeight(store, tree.root);
    ASSERT_TRUE(height.ok()) << height.status().message();
    EXPECT_EQ(height.value(), 3u) << "the root split must add a level, not overflow a node";

    auto root_bytes = store.Get(tree.root);
    ASSERT_TRUE(root_bytes.ok()) << root_bytes.status().message();
    InternalView root(root_bytes.value().bytes());
    EXPECT_EQ(root.level(), 2u);
    EXPECT_EQ(root.entry_count(), 1u) << "a right-split promotes exactly one separator";

    auto leaves = BtreeLeafCount(store, tree.root);
    ASSERT_TRUE(leaves.ok()) << leaves.status().message();
    EXPECT_EQ(leaves.value(), n) << "one leaf per tuple at this payload size";

    // The tree is still correct through the deeper descent - the thing a
    // level growth is most likely to have broken.
    for (std::uint64_t id = 1; id <= n; ++id) {
        auto loc = BtreeLookup(store, tree.root, id);
        ASSERT_TRUE(loc.ok()) << "id " << id << ": " << loc.status().message();
        EXPECT_EQ(MinKeyOf(store, loc.value().page_id), id == 1 ? 0u : id);
    }
}

// ---- Point lookup --------------------------------------------------------

TEST(BtreeTest, EveryInsertedIdIsFoundWhereTheInsertSaidItLanded) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    std::vector<storage::InsertPlacement> placed;
    for (std::uint64_t id = 1; id <= 40; ++id) {
        auto r = tree.Insert(id, kOnePerLeafFiller);
        ASSERT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        placed.push_back(std::move(r.value()));
        placed.back().held.clear();  // the placement only, not its holds
    }

    for (std::uint64_t id = 1; id <= 40; ++id) {
        auto loc = BtreeLookup(store, tree.root, id);
        ASSERT_TRUE(loc.ok()) << "id " << id << ": " << loc.status().message();
        EXPECT_EQ(loc.value().page_id, placed[id - 1].page_id) << "id " << id;
        EXPECT_EQ(loc.value().slot, placed[id - 1].slot) << "id " << id;
    }
}

TEST(BtreeTest, ALocationIsAnAddressAndTheTupleIsReadThroughAFreshRef) {
    // Location used to carry the leaf's bytes out of the descent; under the
    // pin model that span outlived its pin (workplan-pageref.md Shape C),
    // so the field is gone and the contract this test pins is the new one:
    // a lookup answers *where* - page_id and slot - and the reader fetches
    // that page itself, holding the ref for exactly as long as it reads.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    for (std::uint64_t id = 1; id <= 40; ++id) {
        ASSERT_TRUE(tree.Insert(id, kOnePerLeafFiller).ok());
    }

    auto loc = BtreeLookup(store, tree.root, 17);
    ASSERT_TRUE(loc.ok()) << loc.status().message();

    auto fetched = store.GetForRead(loc.value().page_id);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    heap::PageView leaf(fetched.value().bytes());
    auto tuple = leaf.ReadTuple(loc.value().slot);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();
    EXPECT_EQ(IdOf(tuple.value().payload), 17u);
}

TEST(BtreeTest, AnIdThatWasNeverInsertedIsNotFound) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    // Authoritative: these are misses, not "ask the heap scan instead".
    for (std::uint64_t id : {std::uint64_t{41}, std::uint64_t{100}, kMaxKeystoneId}) {
        auto loc = BtreeLookup(store, tree.root, id);
        EXPECT_FALSE(loc.ok()) << "id " << id;
        EXPECT_EQ(loc.status().code(), StatusCode::kNotFound) << "id " << id;
    }
}

TEST(BtreeTest, ALookupFindsALiveIdAmongRetiredSlots) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    // Small tuples: all seven land in the root leaf, so the leaf-local
    // search - binary probes over a slot array with holes in it - is what
    // is under test, not the descent.
    tree.Fill(7, kSmallFiller);

    auto bytes = store.Get(tree.root);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    heap::PageView leaf(bytes.value().bytes());
    ASSERT_EQ(leaf.slot_count(), 7u);

    // Retire slots 1, 2 and 5 (ids 2, 3 and 6). A retired slot carries no
    // key, so a binary probe can land on a hole; the search has to step
    // past it rather than conclude the window is empty.
    for (std::uint16_t slot : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{5}}) {
        Status s = leaf.RetireSlot(slot);
        ASSERT_TRUE(s.ok()) << s.message();
    }

    for (std::uint64_t id : {std::uint64_t{1}, std::uint64_t{4}, std::uint64_t{5},
                             std::uint64_t{7}}) {
        auto loc = BtreeLookup(store, tree.root, id);
        EXPECT_TRUE(loc.ok()) << "id " << id << ": " << loc.status().message();
    }
    for (std::uint64_t id : {std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{6}}) {
        auto loc = BtreeLookup(store, tree.root, id);
        EXPECT_FALSE(loc.ok()) << "id " << id << " was retired";
        EXPECT_EQ(loc.status().code(), StatusCode::kNotFound) << "id " << id;
    }
}

TEST(BtreeTest, ADeleteMarkedTupleIsStillFoundBecauseVisibilityIsNotThisLayersJob) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(5, kSmallFiller);

    auto bytes = store.Get(tree.root);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    heap::PageView leaf(bytes.value().bytes());
    Status s = leaf.DeleteMark(/*slot=*/2, /*trx_id=*/7);
    ASSERT_TRUE(s.ok()) << s.message();

    // Delete-mark leaves the bytes and the key in place; whether the row is
    // visible depends on the reader's snapshot versus trx_id 7, which the
    // tree does not decide. Reporting NotFound here would hide the row from
    // a transaction that must still see it.
    auto loc = BtreeLookup(store, tree.root, 3);
    ASSERT_TRUE(loc.ok()) << loc.status().message();

    auto tuple = heap::PageView(bytes.value().bytes()).ReadTuple(loc.value().slot);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();
    EXPECT_TRUE(tuple.value().deleted);
    EXPECT_EQ(tuple.value().trx_id, 7u) << "trx_id is the deleter";
}

// ---- Ordered scan --------------------------------------------------------

TEST(BtreeTest, AVisitWalksEveryLeafLeftToRightInIdOrder) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    auto rows = ScanAll(store, tree.root);
    ASSERT_EQ(rows.size(), 40u);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].id, i + 1);
    }
}

TEST(BtreeTest, SteppingLeafPagesVisitsExactlyWhatTheWholeTreeWalkDoes) {
    // BtreeVisitLeafPage + BtreeLeftmostLeaf are the page-boundary form
    // the executor's walk loop owns (workplan-crosscore.md P4d-3), under
    // the same contract as heap::ChainVisitOnePage: stepping by returned
    // sibling ids visits the same (leaf, slot) sequence as BtreeVisit,
    // and the rightmost leaf answers kInvalidPageId.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    std::vector<std::pair<PageId, std::uint16_t>> whole;
    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [&](PageId page_id, heap::PageView&,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            whole.emplace_back(page_id, slot);
            return storage::VisitControl::kContinue;
        });
    ASSERT_TRUE(s.ok()) << s.message();

    auto first = BtreeLeftmostLeaf(store, tree.root);
    ASSERT_TRUE(first.ok()) << first.status().message();

    std::vector<std::pair<PageId, std::uint16_t>> stepped;
    std::size_t boundaries = 0;
    PageId cur = first.value();
    while (cur != kInvalidPageId) {
        auto next = BtreeVisitLeafPage(
            store, cur, storage::PageAccess::kRead,
            [&](PageId page_id, heap::PageView&,
                std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                stepped.emplace_back(page_id, slot);
                return storage::VisitControl::kContinue;
            });
        ASSERT_TRUE(next.ok()) << next.status().message();
        cur = next.value();
        ++boundaries;
    }
    EXPECT_EQ(stepped, whole);
    EXPECT_GT(boundaries, 1u) << "test needs a multi-leaf tree to exercise a real boundary";
}

TEST(BtreeTest, AVisitorsErrorStopsTheWalk) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(10, kSmallFiller);

    int visits = 0;
    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [&](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++visits;
            if (visits == 3) return Status::InvalidArgument("stop here");
            return storage::VisitControl::kContinue;
        });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(visits, 3);
}

// ---- Stoppable walks (docs/spec/parser-v2.md I15 rule 4) ----------------------
//
// The leaf-sibling walk gets the same contract as the heap chain, tested
// the same way, because BtreeVisit and ChainVisit exist to be handed the
// same lambda - a divergence in what kStop means between them would be a
// wrong answer on one storage form and not the other.

TEST(BtreeTest, AVisitorCanStopMidLeafAndTheWalkSucceeds) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(10, kSmallFiller);

    int visits = 0;
    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [&](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++visits;
            return visits == 3 ? storage::VisitControl::kStop : storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(visits, 3) << "the walk continued past the stop";
}

TEST(BtreeTest, StoppingOnALeafsLastSlotNeverFetchesTheNextLeaf) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    // One row per leaf, so slot 0 *is* the last slot of its leaf and the
    // stop lands exactly on the sibling-link boundary.
    tree.Fill(4, kOnePerLeafFiller);
    const auto rows = ScanAll(store, tree.root);
    ASSERT_EQ(rows.size(), 4u);
    ASSERT_NE(rows[0].page_id, rows[1].page_id) << "test needs one row per leaf";

    std::vector<PageId> pages_seen;
    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [&](PageId page_id, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            pages_seen.push_back(page_id);
            return storage::VisitControl::kStop;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(pages_seen.size(), 1u) << "the walk crossed the sibling link after kStop";
    EXPECT_EQ(pages_seen.front(), rows[0].page_id);
}

TEST(BtreeTest, StoppingOnTheLastSlotOfTheLastLeafIsIndistinguishableFromFinishing) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(10, kSmallFiller);
    const std::size_t total = ScanAll(store, tree.root).size();
    ASSERT_EQ(total, 10u);

    std::size_t visits = 0;
    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [&](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++visits;
            return visits == total ? storage::VisitControl::kStop
                                   : storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(visits, total);
}

TEST(BtreeTest, AVisitorReturningAnOkStatusInsteadOfContinueIsRefused) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(5, kSmallFiller);

    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            return Status::OK();
        });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("VisitControl"), std::string::npos) << s.message();
}

// ---- Refusals ------------------------------------------------------------

TEST(BtreeTest, DuplicateIdIsRefused) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(5, kSmallFiller);

    // The descent is exact, so this check is complete in a way the heap
    // chain's tail-only one is not: the leaf it landed on is the only page
    // that could hold the id.
    auto dup = tree.Insert(3, kSmallFiller);
    EXPECT_FALSE(dup.ok());
    EXPECT_EQ(dup.status().code(), StatusCode::kAlreadyExists);
}

TEST(BtreeTest, ADuplicateIsRefusedEvenAfterTheTreeHasBranched) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(40, kOnePerLeafFiller);

    // The one that matters: an id whose leaf is no longer the root. If the
    // descent routed to the wrong leaf, this would be accepted and the
    // relation would hold two rows with one tuple key.
    auto dup = tree.Insert(17, kOnePerLeafFiller);
    EXPECT_FALSE(dup.ok());
    EXPECT_EQ(dup.status().code(), StatusCode::kAlreadyExists);
}

TEST(BtreeTest, APayloadWhoseKeystoneDisagreesWithTheIdIsRefused) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    auto r = BtreeInsert(store, tree.root, /*id=*/7, MakeTuple(9, kSmallFiller), /*trx_id=*/1, /*owner_oid=*/0);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption)
        << "two disagreeing copies of a tuple's identity is a defect, not a choice of which wins";
}

// ---- Dividing a full leaf (docs/spec/heap-and-tuple.md §4.1, PK04) ------------
//
// Reached only by a caller-supplied id that sorts inside a full leaf. Until
// the key-mode amendment these inserts were refused outright, because ids
// were monotonic and nothing could produce one.

// Inserts an ascending, gapped run until the *first* leaf is full, and
// returns every id placed. Fullness is detected by the insert that had to
// grow the tree: ascending inserts never fail, they append-split, so a
// "fill until OutOfSpace" loop would run until the page store did. Every id
// but the last therefore sits in the first leaf, which is what a later
// interior insert divides.
std::vector<std::uint64_t> FillFirstLeaf(Tree& tree) {
    std::vector<std::uint64_t> placed;
    for (std::uint64_t id = 100; placed.size() < 500; id += 10) {
        auto r = tree.Insert(id, kSmallFiller);
        EXPECT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        if (!r.ok()) break;
        placed.push_back(id);
        if (r.value().restructured()) break;  // this one opened a second leaf
    }
    EXPECT_GE(placed.size(), 4u) << "need a first leaf with room to divide";
    return placed;
}

TEST(BtreeTest, AFullLeafDividesToMakeRoomForALowerId) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    std::vector<std::uint64_t> placed = FillFirstLeaf(tree);

    // Land in the gap between the first two ids, which routes back into the
    // now-full first leaf. Before PK04 this was an OutOfSpace naming the
    // open split policy.
    const std::uint64_t interior = placed.front() + 5;
    auto divided = tree.Insert(interior, kSmallFiller);
    ASSERT_TRUE(divided.ok()) << divided.status().message();

    // Every id is still reachable - the whole claim of a division.
    auto rows = ScanAll(store, tree.root);
    std::vector<std::uint64_t> want = placed;
    want.push_back(interior);
    std::sort(want.begin(), want.end());

    std::vector<std::uint64_t> got;
    got.reserve(rows.size());
    for (const ScannedRow& row : rows) got.push_back(row.id);
    // **Not sorted before comparing** (BD-R1, BD-S1): a leaf's keyed slots
    // ascend, so the walk is already key order - the incoming tuple lands
    // where it sorts in whichever half took it, not after the half the
    // division wrote back. Red at BD-S0, which appended it last.
    EXPECT_EQ(got, want) << "a division must lose nothing, duplicate nothing, and keep key order";

    std::set<PageId> pages;
    for (const ScannedRow& row : rows) pages.insert(row.page_id);
    EXPECT_GT(pages.size(), 1u);
}

// Every id on a page sorts below every id on the page after it. That is the
// ordering a division must preserve - not order within a page, which
// invariant 4 says nothing about, but the page-wise ordering that makes
// `min_key` pruning sound.
void ExpectPagesKeyOrdered(const std::vector<ScannedRow>& rows) {
    PageId current = kInvalidPageId;
    std::uint64_t highest_before = 0;
    std::uint64_t highest_here = 0;
    for (const ScannedRow& row : rows) {
        if (row.page_id != current) {
            highest_before = highest_here;
            current = row.page_id;
        }
        EXPECT_GT(row.id, highest_before)
            << "id " << row.id << " on page " << row.page_id
            << " sorts below a key on an earlier page";
        highest_here = std::max(highest_here, row.id);
    }
}

TEST(BtreeTest, ADivisionLeavesBothLeavesInsideInvariantsTwoAndThree) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    const PageId original_leaf = tree.root;
    const std::uint64_t root_min_key = MinKeyOf(store, original_leaf);

    std::vector<std::uint64_t> placed = FillFirstLeaf(tree);
    ASSERT_TRUE(tree.Insert(placed.front() + 5, kSmallFiller).ok());

    // Invariant 2: the divided leaf's min_key is untouched. This is what
    // makes a division legal at all - it moves tuples out, it never rewrites
    // a low bound, and a rewritten one would invalidate every reader that
    // pruned by it without a latch.
    EXPECT_EQ(MinKeyOf(store, original_leaf), root_min_key);

    // Invariant 3: no tuple sits below its page's min_key, on either side.
    for (const ScannedRow& row : ScanAll(store, tree.root)) {
        EXPECT_GE(row.id, MinKeyOf(store, row.page_id))
            << "id " << row.id << " is below page " << row.page_id << "'s min_key";
    }
}

TEST(BtreeTest, ADivisionBumpsThePageEpochOfThePageItRebuilt) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    const PageId original_leaf = tree.root;
    std::vector<std::uint64_t> placed = FillFirstLeaf(tree);

    auto before = store.Get(original_leaf);
    ASSERT_TRUE(before.ok());
    const std::uint64_t epoch_before = heap::PageView(before.value().bytes()).PageEpoch();

    ASSERT_TRUE(tree.Insert(placed.front() + 5, kSmallFiller).ok());

    auto after = store.Get(original_leaf);
    ASSERT_TRUE(after.ok());
    // Every tuple on this page changed slot and half of them changed page,
    // which is a relayout in everything but name (§3.1a). Without the bump a
    // Waystone trail entry or Cabin hint recorded before the division would
    // still compare equal and send a reader to a slot holding another row.
    EXPECT_EQ(heap::PageView(after.value().bytes()).PageEpoch(), epoch_before + 1)
        << "the rebuild must carry the epoch forward, not restart it at 1";
}

TEST(BtreeTest, ADividedLeafKeepsADeleteMarkOnTheVersionItMoved) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    std::vector<std::uint64_t> placed = FillFirstLeaf(tree);

    // The highest id still inside the first leaf - `placed.back()` opened the
    // second one - so it is certain to be in the half that moves.
    const std::uint64_t marked = placed[placed.size() - 2];
    {
        auto found = BtreeLookup(store, tree.root, marked);
        ASSERT_TRUE(found.ok()) << found.status().message();
        auto bytes = store.Get(found.value().page_id);
        ASSERT_TRUE(bytes.ok());
        ASSERT_TRUE(heap::PageView(bytes.value().bytes()).DeleteMark(found.value().slot, 99).ok());
    }

    ASSERT_TRUE(tree.Insert(placed.front() + 5, kSmallFiller).ok());

    auto moved = BtreeLookup(store, tree.root, marked);
    ASSERT_TRUE(moved.ok()) << moved.status().message();
    auto bytes = store.Get(moved.value().page_id);
    ASSERT_TRUE(bytes.ok());
    auto tuple = heap::PageView(bytes.value().bytes()).ReadTuple(moved.value().slot);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();
    // Re-inserting the payload alone would resurrect a row that a snapshot
    // has already been told is gone.
    EXPECT_TRUE(tuple.value().deleted) << "the delete mark did not travel with the version";
    EXPECT_EQ(tuple.value().trx_id, 99u) << "the deleter's identity did not travel either";
}

// ---- Dividing a full internal node (workplan-key-mode.md PK09) -----------

TEST(BtreeTest, AFullInternalNodeDividesWhenAnInteriorSeparatorArrives) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // The shape that reaches it: one tuple per leaf, so every insert splits
    // and every split promotes a separator. `kInternalMaxEntries` of those
    // fill the root's internal node; the next interior key has to divide it.
    //
    // Ascending first, spaced so there is room to come back between the
    // keys - this half only builds the full node.
    const std::uint64_t kSpacing = 10;
    for (std::uint64_t k = 1; k <= kInternalMaxEntries + 8; ++k) {
        auto r = tree.Insert(k * kSpacing, kOnePerLeafFiller);
        ASSERT_TRUE(r.ok()) << "id " << k * kSpacing << ": " << r.status().message();
    }

    // Every id must still be reachable before the interesting part, or a
    // failure below is ambiguous.
    for (std::uint64_t k = 1; k <= kInternalMaxEntries + 8; ++k) {
        ASSERT_TRUE(BtreeLookup(store, tree.root, k * kSpacing).ok())
            << "lost id " << k * kSpacing << " while building the full node";
    }

    // Now an interior key. It divides a leaf, and the separator that comes
    // out sorts *inside* the full internal node - the case the cheap
    // right-split cannot serve and which used to be refused outright.
    auto interior = tree.Insert(kSpacing + kSpacing / 2, kOnePerLeafFiller);
    ASSERT_TRUE(interior.ok()) << interior.status().message();

    // Everything is still findable by descent, which is the property an
    // internal division has to preserve: a separator promoted to the wrong
    // side would strand a whole subtree, and only a descent notices.
    for (std::uint64_t k = 1; k <= kInternalMaxEntries + 8; ++k) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, k * kSpacing).ok())
            << "the internal division stranded id " << k * kSpacing;
    }
    EXPECT_TRUE(BtreeLookup(store, tree.root, kSpacing + kSpacing / 2).ok());

    // And the leaf chain still holds them all, in page-wise key order.
    auto rows = ScanAll(store, tree.root);
    EXPECT_EQ(rows.size(), kInternalMaxEntries + 9);
    ExpectPagesKeyOrdered(rows);
}

TEST(BtreeTest, RepeatedInteriorSeparatorsDivideInternalNodesOverAndOver) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // One division proves the mechanism; this proves it composes. After the
    // root's internal node is full, *every* interior key promotes a
    // separator that has to divide a node - and the nodes those divisions
    // create fill and divide in turn, which is what grows the tree past two
    // levels.
    const std::uint64_t kSpacing = 10;
    const std::uint64_t kBuilt = kInternalMaxEntries + 8;
    for (std::uint64_t k = 1; k <= kBuilt; ++k) {
        ASSERT_TRUE(tree.Insert(k * kSpacing, kOnePerLeafFiller).ok());
    }

    std::vector<std::uint64_t> interior;
    for (std::uint64_t k = 1; k <= 200; ++k) {
        const std::uint64_t id = k * kSpacing + kSpacing / 2;
        auto r = tree.Insert(id, kOnePerLeafFiller);
        ASSERT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        interior.push_back(id);
    }

    for (std::uint64_t k = 1; k <= kBuilt; ++k) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, k * kSpacing).ok())
            << "an internal division stranded id " << k * kSpacing;
    }
    for (std::uint64_t id : interior) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, id).ok()) << "lost interior id " << id;
    }

    auto rows = ScanAll(store, tree.root);
    EXPECT_EQ(rows.size(), kBuilt + interior.size());
    ExpectPagesKeyOrdered(rows);
}

TEST(BtreeTest, ALeafHoldingOneOversizedTupleIsRefusedRatherThanDivided) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // One tuple per leaf: there is no division that makes room, because
    // both halves cannot be non-empty. Reported as the space failure it is.
    ASSERT_TRUE(tree.Insert(10, kOnePerLeafFiller).ok());

    auto backwards = tree.Insert(5, kOnePerLeafFiller);
    EXPECT_FALSE(backwards.ok());
    EXPECT_EQ(backwards.status().code(), StatusCode::kOutOfSpace);
    EXPECT_NE(backwards.status().message().find("fewer than two live tuples"), std::string::npos)
        << "the refusal has to name why, or it reads as a split bug: "
        << backwards.status().message();
}

TEST(BtreeTest, ADescendingRunEndsUpFullyOrderedAndFullyReachable) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // The shape a caller-supplied key mode makes possible and a monotonic
    // one cannot: ids arriving strictly backwards, across enough leaves to
    // force repeated divisions.
    std::vector<std::uint64_t> want;
    for (std::uint64_t id = 400; id >= 1; --id) {
        auto r = tree.Insert(id, kSmallFiller);
        ASSERT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        want.push_back(id);
    }
    std::sort(want.begin(), want.end());

    auto rows = ScanAll(store, tree.root);
    std::vector<std::uint64_t> got;
    for (const ScannedRow& row : rows) got.push_back(row.id);
    std::sort(got.begin(), got.end());  // unordered within a page (invariant 4)
    EXPECT_EQ(got, want);

    // The ordering that does have to survive: page by page.
    ExpectPagesKeyOrdered(rows);

    // And every id is still findable by descent, which is the property the
    // separators have to have kept.
    for (std::uint64_t id : want) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, id).ok()) << "lost id " << id;
    }
}

TEST(BtreeTest, AnAppendSplitInsideTheChainKeepsTheLeafToItsRight) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    // One tuple per leaf, so every insert after the first grows the tree
    // and the three leaves are exactly the three ids.
    //
    // The shape: 10 fills the root leaf, 30 appends a second leaf past it,
    // and 20 lands in a full leaf it sorts *above* - so it takes the
    // append-split path, which was only ever reached at the right edge of
    // the chain while ids were monotonic. A caller-supplied id reaches it in
    // the middle, and the new leaf has to inherit the right sibling it is
    // being spliced in front of. Dropping that link truncates the chain, and
    // every leaf past the splice - here the one holding 30 - disappears from
    // every sequential scan while still answering a descent, which is the
    // most silent shape data loss has.
    ASSERT_TRUE(tree.Insert(10, kOnePerLeafFiller).ok());
    ASSERT_TRUE(tree.Insert(30, kOnePerLeafFiller).ok());
    ASSERT_TRUE(tree.Insert(20, kOnePerLeafFiller).ok());

    auto leaves = BtreeLeafCount(store, tree.root);
    ASSERT_TRUE(leaves.ok()) << leaves.status().message();
    EXPECT_EQ(leaves.value(), 3u) << "the leaf chain lost a page to the splice";

    std::vector<std::uint64_t> got;
    for (const ScannedRow& row : ScanAll(store, tree.root)) got.push_back(row.id);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<std::uint64_t>{10, 20, 30}))
        << "a sequential scan must still see every id the tree answers a lookup for";

    for (std::uint64_t id : {10u, 20u, 30u}) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, id).ok()) << "lost id " << id;
    }
}

// A store that refuses one chosen allocation, so a test can stop a grow at
// the point the sim's `page-fail-grow` stopped one. Forwards everything
// else; the base's three pure seams are the whole surface.
class GrowFailingStore final : public storage::PageStore {
public:
    explicit GrowFailingStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        if (countdown_ > 0 && --countdown_ == 0) {
            return Status::OutOfSpace("injected: the device cannot grow");
        }
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        return inner_.GetUnpinned(page_id);
    }

    // Refuse the `n`-th allocation from now, and only that one.
    void FailAllocationNumber(int n) noexcept { countdown_ = n; }

private:
    storage::InMemoryPageStore& inner_;
    int countdown_ = 0;
};

// H9 (`docs/spec/heap-and-tuple.md` section 3.1b): the leaf chain must stay
// ascending even when a grow dies half-way. Found by the simulation, not by
// review - seed 20260826003, `--mode clean --faults io`, whose integrity
// sweep read `min_key 78 does not exceed a predecessor page's max id 183`.
TEST(BtreeTest, AGrowThatDiesPromotingItsSeparatorLeavesTheLeafChainAlone) {
    storage::InMemoryPageStore backing(128);
    GrowFailingStore store(backing);
    Tree tree(store);

    ASSERT_TRUE(tree.Insert(10, kOnePerLeafFiller).ok());
    const PageId root_leaf = tree.root;

    // The next insert needs two pages: the leaf it appends, then the root
    // the promotion grows above it. Refusing the **second** stops the insert
    // after the new leaf exists and before any separator routes to it -
    // exactly where the injected fault stopped the engine in the sim.
    store.FailAllocationNumber(2);
    auto failed = tree.Insert(20, kOnePerLeafFiller);
    ASSERT_FALSE(failed.ok()) << "the refused allocation must fail the insert";
    EXPECT_EQ(failed.status().code(), StatusCode::kOutOfSpace);

    // The whole of the fix. A link written here would put a leaf in the
    // sibling chain that no separator routes to, so the old leaf keeps
    // taking the ids that leaf holds - and the next append splices its new
    // leaf *in front of* the unrouted one, which is how the chain came to
    // descend. An unlinked page costs an allocation and nothing else.
    {
        auto bytes = store.Get(root_leaf);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(heap::PageView(bytes.value().bytes()).next_page_id(), kInvalidPageId)
            << "a leaf whose separator never landed must not be in the sibling chain";
    }
    EXPECT_EQ(tree.root, root_leaf) << "a failed grow must not have repointed the root";

    // And the tree still grows correctly after it: every leaf opens above
    // every id its predecessors hold, which is the property the sim checks
    // and the one the tail-page duplicate check rests on.
    ASSERT_TRUE(tree.Insert(20, kOnePerLeafFiller).ok());
    ASSERT_TRUE(tree.Insert(30, kOnePerLeafFiller).ok());

    PageId current = kInvalidPageId;
    std::uint64_t current_max = 0;
    std::uint64_t predecessors_max = 0;
    bool have_predecessor = false;
    std::vector<std::uint64_t> scanned;
    for (const ScannedRow& row : ScanAll(store, tree.root)) {
        if (row.page_id != current) {
            if (current != kInvalidPageId) {
                predecessors_max = std::max(predecessors_max, current_max);
                have_predecessor = true;
            }
            current = row.page_id;
            current_max = 0;
            if (have_predecessor) {
                EXPECT_GT(MinKeyOf(store, current), predecessors_max)
                    << "leaf " << current << " opens at or below a predecessor's ids";
            }
        }
        current_max = std::max(current_max, row.id);
        scanned.push_back(row.id);
    }
    EXPECT_EQ(scanned, (std::vector<std::uint64_t>{10, 20, 30}))
        << "a scan must read the ids in ascending order, page by page";
}

// A divide that needs two pages - its new leaf and the root it grows - where
// only the first fits the device and the device refuses to grow for the
// second (`git show d83bd502:docs/inflight/bugs/a-fetch-refused-after-a-page-write-leaves-the-
// mutation-half-done.md`). Both are claimed before the leaf is touched, so
// the refusal leaves the tree as it was. Killed by claiming nothing: the
// leaf divides and links its new half, the root is never created, and the
// moved keys are routed to by no parent.
TEST(BtreeTest, ADivideWhoseSecondPageTheDeviceRefusesWritesNothing) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/1, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto opened = storage::DevicePageStore::Open(*device.value(), storage::FrameCapacity{256},
                                                 /*first_new_page_id=*/16);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    storage::DevicePageStore& store = *opened.value();
    Tree tree(store);

    constexpr std::size_t kFiller = 1016;  // seven to a leaf
    for (std::uint64_t id = 10; id <= 70; id += 10) {
        ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << id;
    }
    const PageId root_leaf = tree.root;
    ASSERT_EQ(BtreeHeight(store, root_leaf).value(), 1u) << "the fill already split";

    testing_device::LeaveRoomForOnePage(*device.value(), store);
    const std::uint32_t allocated = store.allocated_pages();

    device.value()->FailNextGrow(Status::IoError("injected: the device cannot grow"));
    auto refused = tree.Insert(15, kFiller);
    device.value()->ClearInjections();
    ASSERT_FALSE(refused.ok()) << "the root's creation needed the device to grow";
    EXPECT_EQ(refused.status().code(), StatusCode::kIoError) << refused.status().message();
    EXPECT_EQ(store.allocated_pages(), allocated) << "a claim was not given back";

    EXPECT_EQ(tree.root, root_leaf);
    {
        auto bytes = store.Get(root_leaf);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(heap::PageView(bytes.value().bytes()).next_page_id(), kInvalidPageId)
            << "the leaf divided before the refusal";
    }
    for (std::uint64_t id = 10; id <= 70; id += 10) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, id).ok()) << "lost id " << id;
    }

    // And with room, the same insert divides and grows a root, the two
    // pages the refused one needed.
    auto grown = tree.Insert(15, kFiller);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    EXPECT_NE(grown.value().new_root, kInvalidPageId);
    for (std::uint64_t id : {10u, 15u, 20u, 30u, 40u, 50u, 60u, 70u}) {
        EXPECT_TRUE(BtreeLookup(store, tree.root, id).ok()) << "lost id " << id;
    }
}

// A split that grows a root re-publishes it through the relation's anchor,
// and a fetch of the anchor that failed there - after the old root is
// marked grown over - left the relation unable to grow. The anchor is pinned
// before the split writes, so a device that cannot read it refuses the
// insert with the tree as it was, and the pin outlives the insert's return.
// Killed by pinning nothing (the insert succeeds and leaves the anchor to a
// fetch that would fail), and by fetching the anchor without keeping it.
TEST(BtreeTest, AGrowthWhoseAnchorCannotBeReadWritesNothing) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    constexpr std::size_t kFiller = 1016;  // seven to a leaf
    PageId root = kInvalidPageId;
    PageId anchor = kInvalidPageId;
    {
        auto first = storage::DevicePageStore::Open(*device.value(), storage::FrameCapacity{256},
                                                    /*first_new_page_id=*/16);
        ASSERT_TRUE(first.ok()) << first.status().message();
        Tree tree(*first.value());
        for (std::uint64_t id = 10; id <= 70; id += 10) {
            ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << id;
        }
        root = tree.root;
        auto made = first.value()->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
        anchor = made.value().first;
        storage::FormatPage(made.value().second.bytes(), PageType::kAnchor);
        made.value().second.Release();
        ASSERT_TRUE(first.value()->Sync().ok());
    }

    // A store with the tree resident and the anchor not: the pin's fault is
    // the insert's one device read.
    auto opened = storage::DevicePageStore::Open(*device.value(), storage::FrameCapacity{256},
                                                 /*first_new_page_id=*/16);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    storage::DevicePageStore& store = *opened.value();
    for (std::uint64_t id = 10; id <= 70; id += 10) {
        ASSERT_TRUE(BtreeLookup(store, root, id).ok()) << id;
    }
    ASSERT_TRUE(store.CreateNew().ok());  // the free map's region, resident too

    device.value()->FailNextRead(Status::IoError("injected: the anchor cannot be read"));
    auto refused = BtreeInsert(store, root, 15, MakeTuple(15, kFiller), /*trx_id=*/1,
                               /*owner_oid=*/0, anchor);
    device.value()->ClearInjections();
    ASSERT_FALSE(refused.ok()) << "the growth's anchor was not fetched before its writes";
    EXPECT_NE(refused.status().message().find("the anchor cannot be read"), std::string::npos)
        << refused.status().message();
    EXPECT_EQ(BtreeHeight(store, root).value(), 1u);
    {
        auto bytes = store.Get(root);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(heap::PageView(bytes.value().bytes()).next_page_id(), kInvalidPageId)
            << "the leaf divided before the refusal";
    }

    auto grown = BtreeInsert(store, root, 15, MakeTuple(15, kFiller), /*trx_id=*/1,
                             /*owner_oid=*/0, anchor);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    EXPECT_NE(grown.value().new_root, kInvalidPageId);
    // The holds dropped, the anchor's pin is what is left, until it is given back.
    grown.value().held.clear();
    EXPECT_EQ(store.live_pins(), 1u) << "the growth did not keep its anchor pinned";
    grown.value().anchor.Release();
    EXPECT_EQ(store.live_pins(), 0u);
}

// A store that runs one action the **second** time a chosen page is
// fetched, and forwards everything else. It puts another core's divide
// exactly where a caller that re-fetches a lookup's page by id would meet
// it: after the descent's fetch, before the caller's read - a window no
// barrier reaches, because it lies between two lines of one caller
// (`btree_race_test.cpp` samples the same window with real threads).
class DivideOnRefetchStore final : public storage::PageStore {
public:
    explicit DivideOnRefetchStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        if (action_ && page_id == watched_ && ++fetches_ == 2) {
            auto action = std::move(action_);  // disarmed first: the action fetches too
            action_ = nullptr;
            fired_ = true;
            action();
        }
        return inner_.GetUnpinned(page_id);
    }

    void OnSecondFetch(PageId page_id, std::function<void()> action) {
        watched_ = page_id;
        fetches_ = 0;
        action_ = std::move(action);
    }
    void Disarm() noexcept { action_ = nullptr; }
    bool fired() const noexcept { return fired_; }

private:
    storage::InMemoryPageStore& inner_;
    PageId watched_ = kInvalidPageId;
    int fetches_ = 0;
    bool fired_ = false;
    std::function<void()> action_;
};

// **A lookup's slot is the row when the caller reads it** (AT-0 item 12,
// marked (a)). A divide re-inserts the staying half of a leaf and moves the
// upper half to a new page (`SplitLeafAndInsert`), so the `(page, slot)` of
// a key in the upper half names something else once it has run. Until the
// item, `BtreeLookup` dropped its leaf at return and every caller re-fetched
// by id: a point `SELECT` then answered zero rows through its residual and
// an FK check decided its verdict on another row. `Location::leaf` is the
// descent's own hold, so there is no second fetch for a divide to precede.
//
// **Mutation**: `BtreeLookup` releases the leaf and re-fetches it by id
// before returning - the pre-fix callers' shape moved into the function.
// The store's action fires on that re-fetch and the read below meets the
// divided leaf, every run.
TEST(BtreeTest, ALookupsSlotIsStillItsRowWhenTheCallerReadsIt) {
    storage::InMemoryPageStore backing(64);
    DivideOnRefetchStore store(backing);
    Tree tree(store);

    // Ascending ids append, so the first leaf fills in key order and the
    // second opens when it is full. 1016 bytes is eight to a leaf.
    constexpr std::size_t kFiller = 1016;
    std::uint64_t id = 0;
    while (true) {
        id += 10;
        ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << "id " << id;
        auto leaf = BtreeSeekLeaf(store, tree.root, 10);
        ASSERT_TRUE(leaf.ok()) << leaf.status().message();
        auto bytes = store.GetForRead(leaf.value());
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        if (heap::PageView(bytes.value().bytes()).next_page_id() != kInvalidPageId) break;
    }
    auto first = BtreeSeekLeaf(store, tree.root, 10);
    ASSERT_TRUE(first.ok()) << first.status().message();
    std::uint64_t upper = 0;  // the first leaf's highest key: it moves in a divide
    {
        auto bytes = store.GetForRead(first.value());
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        heap::PageView leaf(bytes.value().bytes());
        upper = 10 * static_cast<std::uint64_t>(leaf.slot_count());
    }
    ASSERT_GE(upper, 30u) << "the first leaf must hold enough rows to divide";

    // Another core's divide of that leaf: 15 sorts inside it.
    store.OnSecondFetch(first.value(), [&] {
        ASSERT_TRUE(tree.Insert(15, kFiller).ok()) << "the divide must land";
    });

    auto found = BtreeLookup(store, tree.root, upper);
    ASSERT_TRUE(found.ok()) << found.status().message();
    {
        heap::PageView leaf(found.value().leaf.bytes());
        auto payload = leaf.PayloadAt(found.value().slot, leaf.slot_count());
        ASSERT_TRUE(payload.ok()) << "slot " << found.value().slot << " of page "
                                  << found.value().page_id << ": " << payload.status().message();
        auto got = KeystoneIdOfPayload(payload.value());
        ASSERT_TRUE(got.ok()) << got.status().message();
        EXPECT_EQ(got.value(), upper) << "the lookup's slot held another row when it was read";
    }
    EXPECT_FALSE(store.fired()) << "the lookup fetched its own leaf twice";
    found.value().leaf.Release();
    store.Disarm();

    // The divide the cell guards against is real: once it runs, the key has
    // left the page the lookup named.
    if (!store.fired()) ASSERT_TRUE(tree.Insert(15, kFiller).ok());
    auto moved = BtreeLookup(store, tree.root, upper);
    ASSERT_TRUE(moved.ok()) << moved.status().message();
    EXPECT_NE(moved.value().page_id, found.value().page_id)
        << "the divide must move the key, or the cell proves nothing";
}

TEST(BtreeTest, AnIdBelowItsLeafsMinKeyIsRefused) {
    storage::InMemoryPageStore store(128);

    // A descent can only reach a leaf whose min_key is <= the key, because
    // a separator *is* that min_key - so this is a defensive path, reached
    // only if the two ever disagree. It is checked anyway because the cost
    // of not checking is a tuple written below its page's low key, which
    // silently breaks invariant 3 and the pruning that rests on it.
    auto leaf_created = store.CreateNew();
    ASSERT_TRUE(leaf_created.ok()) << leaf_created.status().message();
    auto& [leaf_id, leaf_bytes_ref] = leaf_created.value();
    const std::span<std::byte, kPageSize> leaf_bytes = leaf_bytes_ref.bytes();
    auto leaf = heap::PageView::CreateEmptyAs(leaf_bytes, /*min_key=*/200, PageType::kBtreeLeaf);
    ASSERT_TRUE(leaf.ok()) << leaf.status().message();

    auto root_created = store.CreateNew();
    ASSERT_TRUE(root_created.ok()) << root_created.status().message();
    auto& [root_id, root_bytes_ref] = root_created.value();
    const std::span<std::byte, kPageSize> root_bytes = root_bytes_ref.bytes();
    auto root = InternalView::CreateEmpty(root_bytes, /*level=*/1, /*leftmost_child=*/leaf_id,
                                          /*owner_oid=*/0);
    ASSERT_TRUE(root.ok()) << root.status().message();

    auto r = BtreeInsert(store, root_id, /*id=*/150, MakeTuple(150, kSmallFiller), /*trx_id=*/1, /*owner_oid=*/0);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kOutOfRange);
}

// ---- Corrupt structures are reported, not walked -------------------------

TEST(BtreeTest, AHeapRootReachedThroughTheTreeIsReportedRatherThanParsed) {
    storage::InMemoryPageStore store(128);

    // A heap-clustered relation's root handed to the tree: the body layout
    // is identical, so nothing crashes - it just answers about the wrong
    // storage organization. The page_type byte is the only thing that can
    // tell them apart.
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto& [page_id, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();
    auto page = heap::PageView::CreateEmpty(bytes, 0);
    ASSERT_TRUE(page.ok()) << page.status().message();

    auto loc = BtreeLookup(store, page_id, 1);
    EXPECT_FALSE(loc.ok());
    EXPECT_EQ(loc.status().code(), StatusCode::kCorruption);

    auto height = BtreeHeight(store, page_id);
    EXPECT_FALSE(height.ok());
    EXPECT_EQ(height.status().code(), StatusCode::kCorruption);
}

TEST(BtreeTest, ACyclicChildPointerIsReportedRatherThanDescendedForever) {
    storage::InMemoryPageStore store(128);

    // An internal node whose child is itself. With no depth guard this is
    // an infinite loop inside a request, which is a hung server.
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto& [node_id, node_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> node_bytes = node_bytes_ref.bytes();
    auto node = InternalView::CreateEmpty(node_bytes, /*level=*/1, /*leftmost_child=*/node_id,
                                          /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    auto loc = BtreeLookup(store, node_id, 1);
    EXPECT_FALSE(loc.ok());
    EXPECT_EQ(loc.status().code(), StatusCode::kCorruption);
}

TEST(BtreeTest, ANonLeafReachedThroughASiblingLinkIsReported) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    ASSERT_TRUE(tree.Insert(1, kOnePerLeafFiller).ok());
    ASSERT_TRUE(tree.Insert(2, kOnePerLeafFiller).ok());

    // Point the first leaf's sibling link at the internal root. A scan that
    // trusted the link would read an entry array as a slot directory.
    auto root_internal = store.Get(tree.root);
    ASSERT_TRUE(root_internal.ok()) << root_internal.status().message();
    const PageId first_leaf = InternalView(root_internal.value().bytes()).leftmost_child();

    auto leaf_bytes = store.Get(first_leaf);
    ASSERT_TRUE(leaf_bytes.ok()) << leaf_bytes.status().message();
    heap::PageView(leaf_bytes.value().bytes()).set_next_page_id(tree.root);

    Status s = BtreeVisit(
        store, tree.root, storage::PageAccess::kRead,
        [](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            return storage::VisitControl::kContinue;
        });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

TEST(BtreeTest, ALeafChainThatLinksBackIntoItselfIsCorruptionNotAHang) {
    // The leaf walks' bound is a cycle check, not a 2^20-page budget a
    // relation past 8 GiB of leaves met on every whole walk (the audit's
    // G19): the last leaf linked back to the second is refused, by the visit
    // and by the count alike.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    for (std::uint64_t id = 1; id <= 4; ++id) ASSERT_TRUE(tree.Insert(id, kOnePerLeafFiller).ok());
    auto first = BtreeLeftmostLeaf(store, tree.root);
    ASSERT_TRUE(first.ok()) << first.status().message();
    std::vector<PageId> leaves;
    for (PageId at = first.value(); at != kInvalidPageId;) {
        leaves.push_back(at);
        at = heap::PageView(store.GetForRead(at).value().bytes()).next_page_id();
    }
    ASSERT_GE(leaves.size(), 3u) << "the fixture must span leaves to prove anything";
    // Back to the middle, and back to the walk's origin: the guard's first
    // visit names the origin, so the two shapes take different paths.
    for (const PageId back_to : {leaves[1], leaves[0]}) {
        SCOPED_TRACE(back_to);
        auto last = store.Get(leaves.back());
        ASSERT_TRUE(last.ok()) << last.status().message();
        heap::PageView(last.value().bytes()).set_next_page_id(back_to);
        last.value().Release();

        Status s = BtreeVisit(
            store, tree.root, storage::PageAccess::kRead,
            [](PageId, heap::PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
                return storage::VisitControl::kContinue;
            });
        EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
        EXPECT_NE(s.message().find("revisits"), std::string::npos) << s.message();
        auto count = BtreeLeafCount(store, tree.root);
        ASSERT_FALSE(count.ok());
        EXPECT_NE(count.status().message().find("revisits"), std::string::npos)
            << count.status().message();
    }
}

// ---- InternalView --------------------------------------------------------

TEST(InternalViewTest, ALevelZeroNodeIsRefusedBecauseThatIsALeaf) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();

    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/0,
                                          /*leftmost=*/1, /*owner_oid=*/0);
    EXPECT_FALSE(node.ok());
    EXPECT_EQ(node.status().code(), StatusCode::kInvalidArgument);
}

TEST(InternalViewTest, RoutingSendsKeysBelowTheFirstSeparatorToTheLeftmostChild) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    // A node with no separators routes everything to the leftmost child -
    // the shape a root split produces before its sibling is added.
    EXPECT_EQ(node.value().ChildFor(0), 10u);
    EXPECT_EQ(node.value().ChildFor(kMaxKeystoneId), 10u);

    ASSERT_TRUE(node.value().InsertEntry(/*sep_key=*/100, /*child=*/11).ok());
    ASSERT_TRUE(node.value().InsertEntry(/*sep_key=*/200, /*child=*/12).ok());

    // entries[i] covers [sep_key(i), sep_key(i+1)); the separator is the
    // low key of the subtree, so the boundary key itself goes right.
    EXPECT_EQ(node.value().ChildFor(0), 10u);
    EXPECT_EQ(node.value().ChildFor(99), 10u);
    EXPECT_EQ(node.value().ChildFor(100), 11u);
    EXPECT_EQ(node.value().ChildFor(199), 11u);
    EXPECT_EQ(node.value().ChildFor(200), 12u);
    EXPECT_EQ(node.value().ChildFor(kMaxKeystoneId), 12u);
}

TEST(InternalViewTest, EntriesAreKeptSortedRegardlessOfInsertionOrder) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    // Inserts arrive in ascending order in every tree this engine builds,
    // but the array is binary-searched, so sortedness has to be a property
    // of InsertEntry rather than of its callers.
    for (std::uint64_t sep : {std::uint64_t{300}, std::uint64_t{100}, std::uint64_t{200}}) {
        ASSERT_TRUE(node.value().InsertEntry(sep, static_cast<PageId>(sep)).ok());
    }
    ASSERT_EQ(node.value().entry_count(), 3u);
    for (std::uint16_t i = 0; i < 3; ++i) {
        auto entry = node.value().Entry(i);
        ASSERT_TRUE(entry.ok()) << entry.status().message();
        EXPECT_EQ(entry.value().sep_key, (i + 1) * 100u);
        EXPECT_EQ(entry.value().child, (i + 1) * 100u);
    }
    EXPECT_EQ(node.value().ChildFor(250), 200u);
}

TEST(InternalViewTest, ARepeatedSeparatorIsRefusedBecauseTwoSubtreesCannotShareALowKey) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    ASSERT_TRUE(node.value().InsertEntry(100, 11).ok());
    auto again = node.value().InsertEntry(100, 12);
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.code(), StatusCode::kAlreadyExists);
}

TEST(InternalViewTest, ASeparatorOutsideTheIdSpaceIsRefused) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    // Invariant 7: an id stored outside the tuple header is a zero-extended
    // uint64 whose bits above kKeystoneIdBits are 0.
    auto too_wide = node.value().InsertEntry(kMaxKeystoneId + 1, 11);
    EXPECT_FALSE(too_wide.ok());
    EXPECT_EQ(too_wide.code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(node.value().InsertEntry(kMaxKeystoneId, 11).ok());
}

TEST(InternalViewTest, AFullNodeRefusesAnotherEntryRatherThanOverrunningThePage) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    for (std::uint16_t i = 0; i < kInternalMaxEntries; ++i) {
        ASSERT_TRUE(node.value().InsertEntry(i + 1u, i + 100u).ok()) << "entry " << i;
    }
    EXPECT_TRUE(node.value().IsFull());

    auto overflow = node.value().InsertEntry(kInternalMaxEntries + 1u, 9999);
    EXPECT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.code(), StatusCode::kOutOfSpace);

    // The refusal must not have corrupted the array on the way out.
    EXPECT_EQ(node.value().entry_count(), kInternalMaxEntries);
    auto last = node.value().Entry(kInternalMaxEntries - 1);
    ASSERT_TRUE(last.ok()) << last.status().message();
    EXPECT_EQ(last.value().sep_key, kInternalMaxEntries);

    auto past_end = node.value().Entry(kInternalMaxEntries);
    EXPECT_FALSE(past_end.ok());
    EXPECT_EQ(past_end.status().code(), StatusCode::kOutOfRange);
}

// ---- The walk back up across cores (AT-S16) -----------------------------
//
// A split promotes its separator into the parents its descent recorded.
// The descent held none of them - it releases each node before reading
// the child - and since AT-S5 another core can divide one in between.
// These cells put that divide on the one fetch the walk up starts with
// (`ActOnFetchStore`): the parent's second fetch by the inserting call,
// its first being the descent's own read. `btree_race_test.cpp` asks the
// same question with real threads.
//
// **Mutations** (2026-09-28, 20 runs each, all killed 20/20): `FetchParent`
// accepting the recorded parent without asking it (the first two cells);
// `SecureParents` growing over a marked root (the `*GrewOver*` cells). A
// divide's rebuild dropping the mark, internal or leaf, is killed by
// `AnOldRootKeepsItsMarkWhenItIsDividedLater`. Marking the old root before
// the new root's allocation, where a failed grow leaves a mark with no
// root above it, is killed by `AGrowThatDiesPromotingItsSeparatorLeavesTheLeafChainAlone`
// - found that way, during the stage. Deleting the right-split's record of
// the old root it marks is killed by `AGrowthRecordsTheOldRootItMarks` - a
// mutant AT-S16's review found no cell for.

// Ids k * 10 for k = 1..n, one tuple per leaf: every insert after the first
// splits, and each split promotes one separator.
std::vector<std::uint64_t> FillOnePerLeaf(Tree& tree, std::uint64_t n) {
    std::vector<std::uint64_t> ids;
    for (std::uint64_t k = 1; k <= n; ++k) {
        EXPECT_TRUE(tree.Insert(k * 10, kOnePerLeafFiller).ok()) << "id " << k * 10;
        ids.push_back(k * 10);
    }
    return ids;
}

void ExpectEveryIdFound(storage::PageStore& store, PageId root,
                        const std::vector<std::uint64_t>& ids) {
    for (std::uint64_t id : ids) {
        auto found = BtreeLookup(store, root, id);
        EXPECT_TRUE(found.ok()) << "id " << id << ": " << found.status().message();
    }
}

// **DK's D1: a write descent takes its leaf exclusive in one fetch.** The
// root over the leaves is level 1, so the descent asks for the leaf with
// `Get` at once and never takes it shared first. A mutant that restores the
// shared read and the re-fetch reads "SX"; one that keeps the shared read
// for the write reads "S".
TEST(BtreeTest, AWriteDescentTakesItsLeafExclusiveInOneFetch) {
    storage::InMemoryPageStore backing(64);
    testing_race::ActOnFetchStore store(backing);
    Tree tree(store);
    auto ids = FillOnePerLeaf(tree, 4);  // a level-1 root over four leaves
    auto leaf = BtreeSeekLeaf(store, tree.root, 20);
    ASSERT_TRUE(leaf.ok()) << leaf.status().message();

    store.OnFetch(leaf.value(), /*nth=*/1, [] {});  // records the leaf's fetches from here
    // 25 sorts inside 20's leaf, and a small row fits beside 20's.
    auto placed = tree.Insert(25, /*filler=*/64);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    EXPECT_EQ(leaf.value(), placed.value().page_id) << "25 belongs in 20's leaf";
    EXPECT_EQ("X", store.watched_modes())
        << "the write descent's fetches of its leaf, S shared and X exclusive";
    ids.push_back(25);
    ExpectEveryIdFound(store, tree.root, ids);
}

// **The outrun: a leaf divided before the write descent reads it.** The
// root is read and released before the leaf is asked for, so another
// core's divide can run before A has touched the leaf at all (its one
// fetch, under D1). Only a question of coverage, asked under the leaf's
// exclusive hold, sees that A's key has moved right.
//
// **Mutation**: drop the coverage arm from `DescendTo` (the `break` on a
// leaf that no longer covers the key) and 65 lands in the left leaf, under
// a separator that routes it elsewhere.
TEST(BtreeTest, ALeafDividedBeforeTheWriteDescentReadsItIsNotWrittenPast) {
    storage::InMemoryPageStore backing(64);
    testing_race::ActOnFetchStore store(backing);
    Tree tree(store);
    constexpr std::size_t kFiller = 1016;  // seven to a leaf
    for (std::uint64_t id = 10; id <= 80; id += 10) {
        ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << id;
    }
    auto left = BtreeSeekLeaf(store, tree.root, 10);
    ASSERT_TRUE(left.ok()) << left.status().message();
    auto right = BtreeSeekLeaf(store, tree.root, 80);
    ASSERT_TRUE(right.ok()) << right.status().message();
    ASSERT_NE(left.value(), right.value()) << "the fill must leave a full left leaf beside 80's";

    // B's 15 divides the full left leaf, moving its upper half - 65's
    // place - to a new page, before A's descent fetches the leaf.
    store.OnFetch(left.value(), /*nth=*/1,
                  [&] { ASSERT_TRUE(tree.Insert(15, kFiller).ok()) << "B's divide must land"; });
    const std::uint64_t hops = Contention::Read().of(Tally::kDescentHops);
    auto placed = tree.Insert(65, kFiller);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    ASSERT_TRUE(store.fired()) << "the divide never ran; the cell tested nothing";
    EXPECT_EQ(Contention::Read().of(Tally::kDescentHops) - hops, 1u)
        << "the descent moved right once (D2) rather than starting over";
    EXPECT_NE(left.value(), placed.value().page_id)
        << "65 landed in the leaf that had already given its key away";
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
    ExpectEveryIdFound(store, tree.root, {10, 15, 20, 30, 40, 50, 60, 65, 70, 80});
}

// ---- D2: a descent that moves right instead of restarting (DK-S2) ------
//
// A leaf that no longer covers the key hands the descent its right
// sibling: the failed check proved the key at or above that sibling's
// immutable low key, so the walk right needs no coupling. The cells count
// hops through `sys.meta`'s `descent_hops` tally (process-wide: a delta
// across the cell).
//
// **Mutations**: hop from the named root too (DK-Q10) and the root-leaf
// cell places 85 where it must refuse; take the hop budget to 0 and the
// stale internal root cells are refused again, as before D2.

std::uint64_t Hops() { return Contention::Read().of(Tally::kDescentHops); }

// A stale **internal** root: R, full at level 1, has had a level grown over
// it, and 6800's leaf sits under R's new sibling. A write and a read from
// R land in R's rightmost leaf, which no longer covers 6805 or 6800, and
// move right to the leaf that does - where until D2 every attempt failed
// alike and the statement was refused.
TEST(BtreeTest, AWriteAndALookupFromAStaleInternalRootMoveRightToTheirLeaf) {
    storage::InMemoryPageStore backing(128);
    Tree tree(backing);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    ASSERT_TRUE(tree.Insert(6800, kOnePerLeafFiller).ok());  // grows a level over R
    ASSERT_NE(stale, tree.root) << "the root must have grown, or there is no stale root";
    ids.push_back(6800);

    const std::uint64_t before = Hops();
    // A small row: it fits beside 6800's, so the insert needs no split and
    // no parent - the case D2 answers under a stale root.
    auto placed = BtreeInsert(backing, stale, 6805, MakeTuple(6805, 64), /*trx_id=*/1,
                              /*owner_oid=*/0);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    EXPECT_EQ(Hops() - before, 1u) << "one hop, from R's rightmost leaf to 6800's";
    ids.push_back(6805);

    auto found = BtreeLookup(backing, stale, 6800);
    ASSERT_TRUE(found.ok()) << "a read lookup from the stale root: " << found.status().message();
    EXPECT_EQ(found.value().page_id, placed.value().page_id);
    found.value().leaf.Release();
    EXPECT_EQ(Hops() - before, 2u) << "the lookup's miss hopped once too";

    ExpectEveryIdFound(backing, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(backing, tree.root);
}

// **A split while the hop is between pages.** The hop has released R's
// rightmost leaf and not yet fetched 6800's - the target's second fetch,
// after the coverage check's shared read of it - and another core's 6801
// splits 6800's leaf there, so 6805 belongs to the new page beyond it. The
// check under the new hold fails again, and the walk takes a second hop.
TEST(BtreeTest, ALeafSplitWhileAHopIsBetweenPagesIsWalkedPastToo) {
    storage::InMemoryPageStore backing(128);
    testing_race::ActOnFetchStore store(backing);
    Tree tree(store);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    ASSERT_TRUE(tree.Insert(6800, kOnePerLeafFiller).ok());
    ASSERT_NE(stale, tree.root);
    auto target = BtreeSeekLeaf(store, tree.root, 6800);
    ASSERT_TRUE(target.ok()) << target.status().message();

    store.OnFetch(target.value(), /*nth=*/2, [&] {
        ASSERT_TRUE(tree.Insert(6801, kOnePerLeafFiller).ok()) << "the split must land";
    });
    const std::uint64_t before = Hops();
    auto placed = BtreeInsert(store, stale, 6805, MakeTuple(6805, 64), /*trx_id=*/1,
                              /*owner_oid=*/0);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    ASSERT_TRUE(store.fired()) << "the split never ran; the cell tested nothing";
    EXPECT_EQ(store.watched_modes().substr(0, 2), "SX")
        << "the coverage read of 6800's leaf, then the hop's exclusive fetch of it";
    EXPECT_EQ(Hops() - before, 2u) << "R's leaf to 6800's, then 6800's to 6801's";
    EXPECT_NE(target.value(), placed.value().page_id) << "6805 landed left of 6801";
    for (std::uint64_t id : {6800u, 6801u, 6805u}) ids.push_back(id);
    ExpectEveryIdFound(store, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
}

// **A hop out of the stale root's subtree, then a split.** 6805 is moved
// right from R's rightmost leaf to 6800's, which lies under R's new
// sibling; it is a full-size row, so the leaf must split, and no parent
// at or below R routes to it. `FetchParent` finds none and the insert is
// refused, retryable, with nothing written - never a separator promoted
// into R, which would route 6800's keys through a page no descent from the
// real root reads for them.
TEST(BtreeTest, ASplitFromALeafAHopReachedOutsideTheStaleRootIsRefusedWritingNothing) {
    storage::InMemoryPageStore backing(128);
    Tree tree(backing);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    ASSERT_TRUE(tree.Insert(6800, kOnePerLeafFiller).ok());
    ASSERT_NE(stale, tree.root);
    ids.push_back(6800);
    auto target = BtreeSeekLeaf(backing, tree.root, 6800);
    ASSERT_TRUE(target.ok()) << target.status().message();

    const std::uint64_t before = Hops();
    auto refused = BtreeInsert(backing, stale, 6805, MakeTuple(6805, kOnePerLeafFiller),
                               /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_FALSE(refused.ok()) << "6805 was placed through a stale root's parent";
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();
    EXPECT_GE(Hops() - before, 1u) << "the descent never moved right; the cell tested nothing";
    {
        auto leaf = backing.GetForRead(target.value());
        ASSERT_TRUE(leaf.ok()) << leaf.status().message();
        EXPECT_EQ(heap::PageView(leaf.value().bytes()).next_page_id(), kInvalidPageId)
            << "a refused split spliced a page in after 6800's leaf";
    }
    EXPECT_EQ(StatusCode::kNotFound, BtreeLookup(backing, tree.root, 6805).status().code());
    ExpectEveryIdFound(backing, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(backing, tree.root);
}

// **The budget.** Ten leaves under R's new sibling put 6905's leaf eleven
// hops from R's rightmost: past `kMaxLeafHops`, every attempt from R spends
// its hops and starts over, and the descent is refused as before D2.
TEST(BtreeTest, AWalkRightPastTheHopBudgetStartsOverAndIsRefused) {
    storage::InMemoryPageStore backing(256);
    Tree tree(backing);
    FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    for (std::uint64_t id = 6800; id <= 6900; id += 10) {
        ASSERT_TRUE(tree.Insert(id, kOnePerLeafFiller).ok()) << id;
    }
    ASSERT_NE(stale, tree.root);

    const std::uint64_t before = Hops();
    auto refused = BtreeInsert(backing, stale, 6905, MakeTuple(6905, 64), /*trx_id=*/1,
                               /*owner_oid=*/0);
    ASSERT_FALSE(refused.ok()) << "6905 was placed past the hop budget";
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();
    EXPECT_EQ(Hops() - before,
              static_cast<std::uint64_t>(storage::kMaxLeafHops) *
                  (storage::kMaxDescentRestarts + 1))
        << "every attempt spends its hops, and no more";
}

// **Not from the named root** (DK-Q10, `[quiet-wrong]`). A root that was a
// leaf and has been grown over is still named by a stale memo; a write that
// hopped from it would hold a non-root leaf as `path[0]`, and a split from
// there would grow a level over that leaf. So the write does not hop: it is
// refused, retryable, as before D2, and writes nothing.
TEST(BtreeTest, AWriteFromAStaleRootLeafIsRefusedRatherThanMovedRight) {
    storage::InMemoryPageStore backing(64);
    Tree tree(backing);
    constexpr std::size_t kFiller = 1016;  // seven to a leaf
    for (std::uint64_t id = 10; id <= 70; id += 10) {
        ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << id;
    }
    const PageId stale = tree.root;
    ASSERT_TRUE(tree.Insert(80, kFiller).ok());  // splits the root leaf and grows a level
    ASSERT_NE(stale, tree.root) << "the root must have grown, or there is no stale root";

    const std::uint64_t before = Hops();
    auto refused = BtreeInsert(backing, stale, 85, MakeTuple(85, 64), /*trx_id=*/1,
                               /*owner_oid=*/0);
    ASSERT_FALSE(refused.ok()) << "85 was placed through a stale root leaf, in page "
                               << refused.value().page_id;
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();
    EXPECT_EQ(Hops(), before) << "a write hopped from the named root";
    auto missing = BtreeLookup(backing, tree.root, 85);
    EXPECT_EQ(StatusCode::kNotFound, missing.status().code()) << "a refused insert wrote 85";

    // A read holds no path, so a lookup does move right from the root leaf
    // (DK-Q10 limits writes only): 80 is found through the stale root.
    const std::uint64_t reads = Hops();
    auto found = BtreeLookup(backing, stale, 80);
    ASSERT_TRUE(found.ok()) << "a read lookup from a stale root leaf: " << found.status().message();
    found.value().leaf.Release();
    EXPECT_EQ(Hops() - reads, 1u);
}

// **A `level` that lies is Corruption, never a write into an internal
// node.** A two-level root is poked to say level 1, so a write descent
// takes its internal child exclusive as though it were a leaf; it must
// refuse there rather than parse the node as a slot directory.
TEST(BtreeTest, AWriteDescentRefusesAChildThatIsNotTheLeafItsParentsLevelNamed) {
    storage::InMemoryPageStore backing(128);
    Tree tree(backing);
    FillOnePerLeaf(tree, kInternalMaxEntries + 2);  // the 680th grows a second level
    {
        auto root = backing.Get(tree.root);
        ASSERT_TRUE(root.ok()) << root.status().message();
        ASSERT_EQ(InternalView(root.value().bytes()).level(), 2u);
        const std::uint16_t lie = 1;
        std::memcpy(root.value().bytes().data() + kInternalHeaderOffset + kInternalLevelOffset,
                    &lie, sizeof(lie));
    }
    auto placed = tree.Insert(5, kOnePerLeafFiller);
    ASSERT_FALSE(placed.ok()) << "a write descent placed a row under a lying level";
    EXPECT_EQ(placed.status().code(), StatusCode::kCorruption) << placed.status().message();
}

TEST(BtreeTest, APromotionIntoAParentAnotherCoreDividedLandsInTheHalfHoldingItsLeaf) {
    // **Window 2.** 686 leaves: the root's leftmost child P is full (678
    // separators), and its last leaf holds 6790. A's 6795 appends a leaf
    // after it and promotes 6795 into P. B's 15, an interior key, promotes
    // into full P first and divides it: P keeps the lower half, and the
    // leaf A split moves to P's new sibling. Promoting into the recorded P
    // then puts 6795 where no descent for it looks.
    storage::InMemoryPageStore backing(128);
    testing_race::ActOnFetchStore store(backing);
    Tree tree(store);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 8);

    PageId parent = kInvalidPageId;
    {
        auto root = store.GetForRead(tree.root);
        ASSERT_TRUE(root.ok()) << root.status().message();
        parent = InternalView(root.value().bytes()).leftmost_child();
        auto p = store.GetForRead(parent);
        ASSERT_TRUE(p.ok()) << p.status().message();
        InternalView view(p.value().bytes());
        ASSERT_TRUE(view.IsFull()) << "P must be full, or B's promotion does not divide it";
        ASSERT_EQ(view.level(), 1u);
    }

    constexpr std::uint64_t kA = 6795;
    store.OnFetch(parent, /*nth=*/2, [&] {
        ASSERT_TRUE(tree.Insert(15, kOnePerLeafFiller).ok()) << "B's divide must land";
    });
    auto placed = BtreeInsert(store, tree.root, kA, MakeTuple(kA, kOnePerLeafFiller),
                              /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_TRUE(placed.ok()) << placed.status().message();
    ASSERT_TRUE(store.fired()) << "B's divide never ran; the cell tested nothing";
    ASSERT_EQ(kInvalidPageId, placed.value().new_root);

    auto found = BtreeLookup(store, tree.root, kA);
    ASSERT_TRUE(found.ok()) << "A's row: " << found.status().message();
    EXPECT_EQ(placed.value().page_id, found.value().page_id);
    found.value().leaf.Release();
    ids.push_back(15);
    ExpectEveryIdFound(store, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
}

TEST(BtreeTest, AnInsertWhoseParentAnotherCoreGrewPastIsRefusedBeforeItWritesAnything) {
    // **A root grown under the walk.** 679 leaves: the root R is one full
    // internal node. A's 6800 appends at the tail and would promote into R.
    // B's 15 divides R first and grows a level over it, so the leaf A split
    // is now under R's new sibling - a page A's root cannot reach. What A
    // can do is refuse, retryable, **before** it has written a page; what
    // it must not do is promote 6800 into R's lower half.
    storage::InMemoryPageStore backing(128);
    testing_race::ActOnFetchStore store(backing);
    Tree tree(store);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    {
        auto root = store.GetForRead(stale);
        ASSERT_TRUE(root.ok()) << root.status().message();
        ASSERT_EQ(storage::RawPageType(root.value().bytes()),
                  static_cast<std::uint8_t>(PageType::kBtreeInternal));
        ASSERT_TRUE(InternalView(root.value().bytes()).IsFull());
    }

    constexpr std::uint64_t kA = 6800;
    store.OnFetch(stale, /*nth=*/2, [&] {
        ASSERT_TRUE(tree.Insert(15, kOnePerLeafFiller).ok()) << "B's divide must land";
    });
    auto refused = BtreeInsert(store, stale, kA, MakeTuple(kA, kOnePerLeafFiller),
                               /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_TRUE(store.fired()) << "B's divide never ran; the cell tested nothing";
    ASSERT_NE(stale, tree.root) << "B must have grown a level, or no root is stale";
    ASSERT_FALSE(refused.ok()) << "A placed 6800 through a root that no longer reaches its leaf";
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();

    auto leaves = BtreeLeafCount(store, tree.root);
    ASSERT_TRUE(leaves.ok()) << leaves.status().message();
    EXPECT_EQ(kInternalMaxEntries + 2u, leaves.value()) << "the refused insert wrote a leaf";
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);

    // The client's retry reads the current root and lands.
    ASSERT_TRUE(tree.Insert(kA, kOnePerLeafFiller).ok());
    ids.push_back(15);
    ids.push_back(kA);
    ExpectEveryIdFound(store, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
}

TEST(BtreeTest, ARootAnotherCoreGrewOverIsNotGrownOverASecondTime) {
    // **The fork.** A core whose memo still names R - B grew a level over
    // it, and a memo drops only at the next task boundary - divides R and
    // would grow a second root over it. Whichever root is published last,
    // the other one's half is gone. B's 6800 appends and grows R_B over the
    // full R without touching R's entries; A's 15 then needs R divided.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    auto ids = FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId stale = tree.root;
    ASSERT_TRUE(tree.Insert(6800, kOnePerLeafFiller).ok());
    ASSERT_NE(stale, tree.root) << "B's append must grow a level over R";

    auto refused = BtreeInsert(store, stale, 15, MakeTuple(15, kOnePerLeafFiller),
                               /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_FALSE(refused.ok()) << "A grew root " << refused.value().new_root
                               << " over a page B had already grown one over";
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();

    auto leaves = BtreeLeafCount(store, tree.root);
    ASSERT_TRUE(leaves.ok()) << leaves.status().message();
    EXPECT_EQ(kInternalMaxEntries + 2u, leaves.value()) << "the refused insert wrote a leaf";
    ASSERT_TRUE(tree.Insert(15, kOnePerLeafFiller).ok()) << "the retry, from the current root";
    ids.push_back(6800);
    ids.push_back(15);
    ExpectEveryIdFound(store, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
}

TEST(BtreeTest, ALeafRootAnotherCoreGrewOverIsNotGrownOverASecondTime) {
    // The same fork one level down, on a tree's first growth: the root is a
    // full leaf, B's append grows a level over it without moving a tuple,
    // and A - still naming the leaf as the root - divides it for a key
    // inside it.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    constexpr std::size_t kFiller = 1016;  // eight to a leaf
    std::vector<std::uint64_t> ids;
    PageId stale = tree.root;
    for (std::uint64_t id = 10; stale == tree.root; id += 10) {
        ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << "id " << id;
        ids.push_back(id);
    }
    ASSERT_GE(ids.size(), 3u) << "the leaf must hold enough rows to divide";

    auto refused = BtreeInsert(store, stale, 15, MakeTuple(15, kFiller), /*trx_id=*/1,
                               /*owner_oid=*/0);
    ASSERT_FALSE(refused.ok()) << "A grew root " << refused.value().new_root
                               << " over a leaf B had already grown one over";
    EXPECT_EQ(StatusCode::kTxnConflict, refused.status().code()) << refused.status().message();

    ASSERT_TRUE(tree.Insert(15, kFiller).ok()) << "the retry, from the current root";
    ids.push_back(15);
    ExpectEveryIdFound(store, tree.root, ids);
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
}

TEST(BtreeTest, AnOldRootKeepsItsMarkWhenItIsDividedLater) {
    // An old root is an ordinary node once grown over, and a divide
    // **rebuilds** the page it divides - which zeroes the header flags the
    // mark lives in. A core whose memo still names the old root is what the
    // mark is for, and nothing stops it outliving the divide.
    //
    // The leaf: B's append grows a level over the full root leaf, a
    // current-root insert of 15 divides it, and A - still naming it as the
    // root - fills it back up and divides it for a key inside it.
    {
        storage::InMemoryPageStore store(128);
        Tree tree(store);
        constexpr std::size_t kFiller = 1016;  // eight to a leaf
        std::vector<std::uint64_t> ids;
        const PageId stale = tree.root;
        for (std::uint64_t id = 10; stale == tree.root; id += 10) {
            ASSERT_TRUE(tree.Insert(id, kFiller).ok()) << "id " << id;
            ids.push_back(id);
        }
        ASSERT_TRUE(tree.Insert(15, kFiller).ok());  // divides the old root leaf
        ids.push_back(15);
        {
            auto bytes = store.GetForRead(stale);
            ASSERT_TRUE(bytes.ok()) << bytes.status().message();
            ASSERT_TRUE(heap::PageView(bytes.value().bytes()).grown_over())
                << "the divide's rebuild dropped the old root leaf's mark";
        }
        Status last = Status::OK();
        for (std::uint64_t id = 11; id < 15; ++id) {
            auto r = BtreeInsert(store, stale, id, MakeTuple(id, kFiller), /*trx_id=*/1,
                                 /*owner_oid=*/0);
            if (!r.ok()) {
                last = r.status();
                break;
            }
            ASSERT_EQ(kInvalidPageId, r.value().new_root)
                << "id " << id << " grew a second root over the old root leaf";
            ids.push_back(id);
        }
        EXPECT_EQ(StatusCode::kTxnConflict, last.code())
            << "the refill never reached a divide; the cell tested nothing";
        ExpectEveryIdFound(store, tree.root, ids);
        testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(store, tree.root);
    }
    // The internal node: B's append grows a level over the full root R,
    // and a current-root insert of 15 divides R.
    {
        storage::InMemoryPageStore store(128);
        Tree tree(store);
        FillOnePerLeaf(tree, kInternalMaxEntries + 1);
        const PageId stale = tree.root;
        ASSERT_TRUE(tree.Insert(6800, kOnePerLeafFiller).ok());
        ASSERT_TRUE(tree.Insert(15, kOnePerLeafFiller).ok());
        auto bytes = store.GetForRead(stale);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        InternalView view(bytes.value().bytes());
        ASSERT_FALSE(view.IsFull()) << "15 must have divided the old root";
        EXPECT_TRUE(view.grown_over()) << "the divide's rebuild dropped the old root's mark";
    }
}

TEST(BtreeTest, AGrowthRecordsTheOldRootItMarks) {
    // The mark is a write, so the old root owes redo an image. A divided
    // root records itself and a leaf root's split path records the leaf;
    // a right-split at a full internal root writes nothing else into it, so
    // that branch records it for the mark alone - every page an insert
    // writes is described by a record, or redo rebuilds it without the
    // write.
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    FillOnePerLeaf(tree, kInternalMaxEntries + 1);
    const PageId old_root = tree.root;
    auto grown = tree.Insert(6800, kOnePerLeafFiller);
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    ASSERT_NE(old_root, tree.root) << "the append must grow a level over the full root";

    bool recorded = false;
    for (const PageId changed : grown.value().changes()) {
        if (changed == old_root) recorded = true;
    }
    EXPECT_TRUE(recorded) << "the marked old root has no image for redo";
    auto bytes = store.GetForRead(old_root);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_TRUE(InternalView(bytes.value().bytes()).grown_over());
}

// ---- One door, and its duplicate (BD-R5, BD-R6) ---------------------------
//
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`. BB-R1's two
// doors - an issue and an admission run under the rightmost leaf's hold -
// are deleted on a btree with BB-R1 itself (W10): `BtreeInsert` is the one
// door, any id placed where it sorts. What it refuses for the key is a
// duplicate, live or delete-marked, and the text says which.

TEST(BtreeTest, ADuplicateKeyIsRefusedAndADeletedOneSaysItIsBoundOnce) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    tree.Fill(3, kSmallFiller);

    auto live = tree.Insert(2, kSmallFiller);
    ASSERT_FALSE(live.ok());
    EXPECT_EQ(live.status().code(), StatusCode::kAlreadyExists) << live.status().message();
    EXPECT_NE(live.status().message().find("duplicate tuple key 2"), std::string::npos)
        << live.status().message();

    {
        auto at = BtreeLookup(store, tree.root, 3, storage::PageAccess::kWrite);
        ASSERT_TRUE(at.ok()) << at.status().message();
        ASSERT_TRUE(heap::PageView(at.value().leaf.bytes()).DeleteMark(at.value().slot, 9).ok());
    }
    auto deleted = tree.Insert(3, kSmallFiller);
    ASSERT_FALSE(deleted.ok());
    EXPECT_EQ(deleted.status().code(), StatusCode::kAlreadyExists);
    EXPECT_NE(deleted.status().message().find(
                  "a row with this key was deleted; PURGE frees its key"),
              std::string::npos)
        << deleted.status().message();
    EXPECT_EQ(ScanAll(store, tree.root).size(), 3u);
}

// ---- BD-S1: a leaf in key order whatever order ids arrive in (BD-R1, BD-R2) -
//
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`. A row takes
// the slot its key sorts to, so a walk yields every leaf's ids ascending -
// live, delete-marked, on a leaf that divided. Red at BD-S0, where a leaf
// places at its slot count and the divide appends its incoming row last.

std::vector<std::uint64_t> WalkIds(storage::PageStore& store, PageId root) {
    std::vector<std::uint64_t> ids;
    for (const ScannedRow& row : ScanAll(store, root)) ids.push_back(row.id);
    return ids;
}

TEST(BtreeTest, ALeafTakesEachIdAtTheSlotItSortsTo) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);
    for (std::uint64_t id : {30, 10, 50, 20, 40}) {
        ASSERT_TRUE(tree.Insert(id, kSmallFiller).ok()) << id;
    }
    EXPECT_EQ(WalkIds(store, tree.root), (std::vector<std::uint64_t>{10, 20, 30, 40, 50}));

    // And the lookup finds each at the slot the walk saw it in.
    for (std::uint64_t id : {10, 20, 30, 40, 50}) {
        auto at = BtreeLookup(store, tree.root, id);
        ASSERT_TRUE(at.ok()) << id << ": " << at.status().message();
        heap::PageView leaf(at.value().leaf.bytes());
        auto tuple = leaf.ReadTuple(at.value().slot);
        ASSERT_TRUE(tuple.ok());
        EXPECT_EQ(IdOf(tuple.value().payload), id);
    }
}

TEST(BtreeTest, ADescendingRunAcrossManyDividesLeavesEveryLeafInKeyOrder) {
    storage::InMemoryPageStore store(256);
    Tree tree(store);
    const std::uint64_t kRows = 600;
    for (std::uint64_t id = kRows; id >= 1; --id) {
        auto r = tree.Insert(id, kSmallFiller);
        ASSERT_TRUE(r.ok()) << id << ": " << r.status().message();
    }
    std::vector<std::uint64_t> want;
    for (std::uint64_t id = 1; id <= kRows; ++id) want.push_back(id);
    EXPECT_EQ(WalkIds(store, tree.root), want);
}

TEST(BtreeTest, AFullRightmostLeafSplitsAtTheInsertionPoint) {
    // BD-Q11 (a): an id below the top of a full rightmost leaf opens the new
    // leaf, the keys above it move with it, and every key below it stays -
    // the left leaf stays as full as it was. A median cut would move half.
    storage::InMemoryPageStore store(64);
    std::size_t capacity = 0;  // rows of kSmallFiller one leaf holds
    {
        Tree probe(store);
        for (std::uint64_t id = 10;; id += 10) {
            auto r = probe.Insert(id, kSmallFiller);
            ASSERT_TRUE(r.ok()) << r.status().message();
            if (r.value().restructured()) break;
            ++capacity;
        }
    }
    ASSERT_GE(capacity, 8u);

    storage::InMemoryPageStore fresh(64);
    Tree tree(fresh);
    for (std::uint64_t k = 1; k <= capacity; ++k) {
        ASSERT_TRUE(tree.Insert(k * 10, kSmallFiller).ok());
    }
    // Below the top three keys of the full, rightmost leaf.
    const std::uint64_t id = (capacity - 3) * 10 + 5;
    auto split = tree.Insert(id, kSmallFiller);
    ASSERT_TRUE(split.ok()) << split.status().message();
    ASSERT_TRUE(split.value().restructured());

    const std::vector<ScannedRow> rows = ScanAll(fresh, tree.root);
    ASSERT_EQ(rows.size(), capacity + 1);
    const PageId left = rows.front().page_id;
    std::size_t on_left = 0;
    for (const ScannedRow& row : rows) on_left += row.page_id == left ? 1 : 0;
    EXPECT_EQ(on_left, capacity - 3) << "the rightmost leaf divided at the median";
    EXPECT_EQ(rows[on_left].id, id) << "the incoming id does not open the new leaf";
}

}  // namespace
}  // namespace kds::btree
