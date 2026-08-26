#include "kds/storage/btree/btree.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

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
            split = r.value();
            root = r.value().new_root;
            grew = true;
        }
    }
    ASSERT_TRUE(grew) << "the tree never grew a level";

    for (const auto& change : split.changes()) {
        auto bytes = store.Get(change.page_id);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()), 4001u)
            << "page " << change.page_id;
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

    // The new leaf first: PAGE_INIT describes it completely, and the
    // HEAP_INSERT the caller emits afterwards fills it.
    EXPECT_EQ(changes[0].page_id, split.value().page_id);
    EXPECT_TRUE(changes[0].is_new_page);
    EXPECT_EQ(changes[0].min_key, 2u);

    // Then the old leaf, whose forward link now reaches it. Not a new page,
    // so it needs a FULL_PAGE_IMAGE - no record type describes a link edit.
    EXPECT_EQ(changes[1].page_id, first_leaf);
    EXPECT_FALSE(changes[1].is_new_page);

    // A brand-new internal node is reported too, and also as not-new: an
    // entry array has no PAGE_INIT that describes it.
    for (std::size_t i = 2; i < changes.size(); ++i) {
        EXPECT_FALSE(changes[i].is_new_page) << "change " << i;
    }
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
        placed.push_back(r.value());
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

TEST(BtreeTest, ALookupFindsAnIdInALeafWhoseSlotsAreOutOfOrder) {
    storage::InMemoryPageStore store(128);

    // Leaves are in ascending key order in every tree this engine builds -
    // ids are issued monotonically and the split path refuses to divide a
    // page - which is what makes the leaf-local binary search correct. But
    // nothing *depends* on that: a search that finds nothing falls through
    // to a linear scan, so an out-of-order leaf costs probes and still
    // returns the right answer.
    //
    // Only reachable by writing the slots directly, because BtreeInsert
    // would refuse the descending sequence (OutOfSpace, naming the open
    // split policy). That is the point - this covers the day some future
    // relayout or a recovered page makes the assumption false, so the
    // fallback is not silently dead code that has already rotted.
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto& [root_id, root_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> root_bytes = root_bytes_ref.bytes();
    auto leaf = heap::PageView::CreateEmptyAs(root_bytes, /*min_key=*/0, PageType::kBtreeLeaf);
    ASSERT_TRUE(leaf.ok()) << leaf.status().message();

    const std::vector<std::uint64_t> ids = {50, 10, 40, 20, 30};
    for (std::uint64_t id : ids) {
        auto slot = leaf.value().InsertTuple(MakeTuple(id, kSmallFiller), /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(slot.ok()) << "id " << id << ": " << slot.status().message();
    }

    for (std::uint64_t id : ids) {
        auto loc = BtreeLookup(store, root_id, id);
        ASSERT_TRUE(loc.ok()) << "id " << id << ": " << loc.status().message();
        EXPECT_EQ(loc.value().page_id, root_id);

        auto tuple = heap::PageView(root_bytes).ReadTuple(loc.value().slot);
        ASSERT_TRUE(tuple.ok()) << tuple.status().message();
        EXPECT_EQ(IdOf(tuple.value().payload), id) << "found the wrong slot for id " << id;
    }

    auto absent = BtreeLookup(store, root_id, 25);
    EXPECT_FALSE(absent.ok());
    EXPECT_EQ(absent.status().code(), StatusCode::kNotFound);
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
    // relation would hold two rows with one primary key.
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
    // Sorted before comparing, deliberately: **within** a page tuples are
    // unordered (invariant 4), and a division appends the incoming tuple
    // after the half it wrote back, so it lands at the end of its page's
    // slots. What must hold is that nothing was lost or duplicated.
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, want) << "a division must lose nothing and duplicate nothing";

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

TEST(BtreeTest, ADivisionBumpsTheRelayoutEpochOfThePageItRebuilt) {
    storage::InMemoryPageStore store(128);
    Tree tree(store);

    const PageId original_leaf = tree.root;
    std::vector<std::uint64_t> placed = FillFirstLeaf(tree);

    auto before = store.Get(original_leaf);
    ASSERT_TRUE(before.ok());
    const std::uint64_t epoch_before = heap::PageView(before.value().bytes()).RelayoutEpoch();

    ASSERT_TRUE(tree.Insert(placed.front() + 5, kSmallFiller).ok());

    auto after = store.Get(original_leaf);
    ASSERT_TRUE(after.ok());
    // Every tuple on this page changed slot and half of them changed page,
    // which is a relayout in everything but name (§3.1a). Without the bump a
    // Waystone trail entry or Cabin hint recorded before the division would
    // still compare equal and send a reader to a slot holding another row.
    EXPECT_EQ(heap::PageView(after.value().bytes()).RelayoutEpoch(), epoch_before + 1)
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

TEST(InternalViewTest, ASeparatorOutsideThe40BitIdRangeIsRefused) {
    storage::InMemoryPageStore store(128);
    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto node = InternalView::CreateEmpty(created.value().second.bytes(), /*level=*/1,
                                          /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();

    // Invariant 6: an id stored outside the tuple header is a zero-extended
    // uint64 whose upper 24 bits are 0.
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

}  // namespace
}  // namespace kds::btree
