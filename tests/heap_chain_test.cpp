#include "kds/storage/heap/heap_chain.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"

// The heap as a chain of pages: growth at the tail, the invariants that
// make tail-append correct, and the walk that reads it back.
//
// The property under test throughout is the ordering one heap_chain.hpp
// rests on - every page's ids are strictly below the next page's min_key -
// because that is what makes the O(1) duplicate check complete and what a
// future B+ tree / min_key pruning pass will rely on.

namespace kds::heap {
namespace {

// The chain's head, mirroring what Catalog::CreateTable does for a
// relation: a formatted, empty heap page with min_key 0.
PageId MakeHead(storage::PageStore& store) {
    auto created = store.CreateNew();
    EXPECT_TRUE(created.ok()) << created.status().message();
    auto [page_id, bytes] = created.value();
    auto page = PageView::CreateEmpty(bytes, 0);
    EXPECT_TRUE(page.ok()) << page.status().message();
    return page_id;
}

// A tuple payload: the Keystone word carrying `id`, then `filler` bytes of
// body. Big fillers fill a page fast, which is the point.
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
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(payload[i]));
    }
    return Keystone::Decode(v).id;
}

// Inserts ids 1..n and returns where each one landed.
std::vector<ChainInsertResult> FillChain(storage::PageStore& store, PageId head, std::uint64_t n,
                                          std::size_t filler) {
    std::vector<ChainInsertResult> placed;
    for (std::uint64_t id = 1; id <= n; ++id) {
        auto r = ChainInsert(store, head, id, MakeTuple(id, filler), /*trx_id=*/1);
        EXPECT_TRUE(r.ok()) << "id " << id << ": " << r.status().message();
        if (!r.ok()) break;
        placed.push_back(r.value());
    }
    return placed;
}

TEST(HeapChainTest, AFreshChainIsOnePageAndItsOwnTail) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    auto tail = ChainTail(store, head);
    ASSERT_TRUE(tail.ok()) << tail.status().message();
    EXPECT_EQ(tail.value(), head);

    auto len = ChainLength(store, head);
    ASSERT_TRUE(len.ok()) << len.status().message();
    EXPECT_EQ(len.value(), 1u);
}

TEST(HeapChainTest, InsertsStayOnOnePageUntilItFills) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    // 64-byte tuples: a few hundred fit in one 8 KB page, so ten cannot
    // possibly grow the chain.
    auto placed = FillChain(store, head, 10, /*filler=*/56);
    ASSERT_EQ(placed.size(), 10u);
    for (const auto& p : placed) {
        EXPECT_EQ(p.page_id, head);
        EXPECT_FALSE(p.grew_chain);
    }

    auto len = ChainLength(store, head);
    ASSERT_TRUE(len.ok()) << len.status().message();
    EXPECT_EQ(len.value(), 1u);
}

TEST(HeapChainTest, AFullTailGrowsTheChainInsteadOfFailing) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    // 1 KB tuples: the head takes a handful, then the chain must grow.
    // Before heap_chain.hpp this is exactly where INSERT returned
    // OutOfSpace and a table stopped accepting rows forever.
    auto placed = FillChain(store, head, 40, /*filler=*/1016);
    ASSERT_EQ(placed.size(), 40u);

    std::uint32_t growths = 0;
    for (const auto& p : placed) {
        if (p.grew_chain) ++growths;
    }
    EXPECT_GT(growths, 0u) << "40 KB of tuples must not fit in one 8 KB page";

    auto len = ChainLength(store, head);
    ASSERT_TRUE(len.ok()) << len.status().message();
    EXPECT_EQ(len.value(), growths + 1);
}

TEST(HeapChainTest, EveryTupleIsReadableBackInIdOrder) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 40, /*filler=*/1016);

    std::vector<std::uint64_t> seen;
    Status s = ChainVisit(store, head, storage::PageAccess::kRead,
                          [&](PageId, PageView& page, std::uint16_t slot) -> Status {
                              auto tuple = page.ReadTuple(slot);
                              if (!tuple.ok()) return Status::OK();
                              seen.push_back(IdOf(tuple.value().payload));
                              return Status::OK();
                          });
    ASSERT_TRUE(s.ok()) << s.message();

    ASSERT_EQ(seen.size(), 40u);
    // Chain order is id order here: ids are issued in increasing order and
    // every insert appends, so nothing reorders them.
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], i + 1);
    }
}

TEST(HeapChainTest, EachPagesIdsAreBelowTheNextPagesMinKey) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 60, /*filler=*/1016);

    // The ordering property in full: walk the chain page by page, and
    // check every id on a page against the *next* page's min_key. This is
    // what makes the tail-only duplicate check complete, and what min_key
    // range pruning will need.
    std::vector<PageId> pages;
    std::vector<std::uint64_t> min_keys;
    std::vector<std::vector<std::uint64_t>> ids_per_page;

    Status s = ChainVisit(store, head, storage::PageAccess::kRead,
                          [&](PageId page_id, PageView& page, std::uint16_t slot) -> Status {
                              if (pages.empty() || pages.back() != page_id) {
                                  pages.push_back(page_id);
                                  min_keys.push_back(page.min_key());
                                  ids_per_page.emplace_back();
                              }
                              auto tuple = page.ReadTuple(slot);
                              if (!tuple.ok()) return Status::OK();
                              ids_per_page.back().push_back(IdOf(tuple.value().payload));
                              return Status::OK();
                          });
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_GT(pages.size(), 1u) << "test needs a multi-page chain to mean anything";

    for (std::size_t p = 0; p < pages.size(); ++p) {
        for (std::uint64_t id : ids_per_page[p]) {
            // Invariant 3: nothing below its own page's min_key.
            EXPECT_GE(id, min_keys[p]) << "page " << pages[p];
            // ...and nothing at or above the next page's min_key.
            if (p + 1 < pages.size()) {
                EXPECT_LT(id, min_keys[p + 1]) << "page " << pages[p];
            }
        }
    }
}

TEST(HeapChainTest, ANewPagesMinKeyIsTheIdThatCausedTheGrowth) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    std::uint64_t id = 1;
    ChainInsertResult growth{};
    for (; id <= 200; ++id) {
        auto r = ChainInsert(store, head, id, MakeTuple(id, 1016), /*trx_id=*/1);
        ASSERT_TRUE(r.ok()) << r.status().message();
        if (r.value().grew_chain) {
            growth = r.value();
            break;
        }
    }
    ASSERT_TRUE(growth.grew_chain) << "chain never grew";

    auto bytes = store.Get(growth.page_id);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_EQ(PageView(bytes.value()).min_key(), id);
}

TEST(HeapChainTest, TheOldTailIsLinkedToTheNewOneAndKeepsItsMinKey) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    auto placed = FillChain(store, head, 40, /*filler=*/1016);
    ASSERT_EQ(placed.size(), 40u);

    auto head_bytes = store.Get(head);
    ASSERT_TRUE(head_bytes.ok()) << head_bytes.status().message();
    PageView head_page(head_bytes.value());

    EXPECT_NE(head_page.next_page_id(), kInvalidPageId) << "head must link on";
    // Invariant 2: growth never rewrites an existing page's min_key.
    EXPECT_EQ(head_page.min_key(), 0u);

    auto tail = ChainTail(store, head);
    ASSERT_TRUE(tail.ok()) << tail.status().message();
    EXPECT_NE(tail.value(), head);
    EXPECT_EQ(placed.back().page_id, tail.value());
}

TEST(HeapChainTest, DuplicateIdIsRefused) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 5, /*filler=*/56);

    auto dup = ChainInsert(store, head, 5, MakeTuple(5, 56), /*trx_id=*/1);
    EXPECT_FALSE(dup.ok());
    EXPECT_EQ(dup.status().code(), StatusCode::kAlreadyExists);
}

TEST(HeapChainTest, AnIdBelowTheTailsMinKeyIsRefused) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 40, /*filler=*/1016);

    auto tail = ChainTail(store, head);
    ASSERT_TRUE(tail.ok()) << tail.status().message();
    auto tail_bytes = store.Get(tail.value());
    ASSERT_TRUE(tail_bytes.ok()) << tail_bytes.status().message();
    const std::uint64_t tail_min_key = PageView(tail_bytes.value()).min_key();
    ASSERT_GT(tail_min_key, 1u) << "test needs a grown chain";

    // A sequence that went backwards. Writing this tuple would either
    // violate invariant 3 or hide a duplicate on an earlier page; both are
    // worse than refusing.
    auto backwards = ChainInsert(store, head, 1, MakeTuple(1, 1016), /*trx_id=*/1);
    EXPECT_FALSE(backwards.ok());
    EXPECT_EQ(backwards.status().code(), StatusCode::kOutOfRange);
}

TEST(HeapChainTest, APayloadWhoseKeystoneDisagreesWithTheIdIsRefused) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    auto mismatched = ChainInsert(store, head, 7, MakeTuple(9, 56), /*trx_id=*/1);
    EXPECT_FALSE(mismatched.ok());
    EXPECT_EQ(mismatched.status().code(), StatusCode::kCorruption);
}

TEST(HeapChainTest, ACyclicChainIsReportedRatherThanLoopedOn) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    // Two pages pointing at each other: a walk with no guard never
    // returns, which inside a request is a hung server.
    auto second = store.CreateNew();
    ASSERT_TRUE(second.ok()) << second.status().message();
    auto [second_id, second_bytes] = second.value();
    auto second_page = PageView::CreateEmpty(second_bytes, 0);
    ASSERT_TRUE(second_page.ok()) << second_page.status().message();

    auto head_bytes = store.Get(head);
    ASSERT_TRUE(head_bytes.ok()) << head_bytes.status().message();
    PageView(head_bytes.value()).set_next_page_id(second_id);
    second_page.value().set_next_page_id(head);

    auto tail = ChainTail(store, head);
    EXPECT_FALSE(tail.ok());
    EXPECT_EQ(tail.status().code(), StatusCode::kCorruption);
}

TEST(HeapChainTest, AVisitorsErrorStopsTheWalk) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 10, /*filler=*/56);

    int visits = 0;
    Status s = ChainVisit(store, head, storage::PageAccess::kRead,
                          [&](PageId, PageView&, std::uint16_t) -> Status {
        ++visits;
        if (visits == 3) return Status::InvalidArgument("stop here");
        return Status::OK();
    });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(visits, 3);
}

}  // namespace
}  // namespace kds::heap
