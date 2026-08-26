#include "kds/storage/heap/heap_chain.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

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
    auto& [page_id, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();
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
        auto r = ChainInsert(store, head, id, MakeTuple(id, filler), /*trx_id=*/1, /*owner_oid=*/0);
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
    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView& page, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) return storage::VisitControl::kContinue;
            seen.push_back(IdOf(tuple.value().payload));
            return storage::VisitControl::kContinue;
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

    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId page_id, PageView& page, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (pages.empty() || pages.back() != page_id) {
                pages.push_back(page_id);
                min_keys.push_back(page.min_key());
                ids_per_page.emplace_back();
            }
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) return storage::VisitControl::kContinue;
            ids_per_page.back().push_back(IdOf(tuple.value().payload));
            return storage::VisitControl::kContinue;
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
        auto r = ChainInsert(store, head, id, MakeTuple(id, 1016), /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(r.ok()) << r.status().message();
        if (r.value().grew_chain) {
            growth = r.value();
            break;
        }
    }
    ASSERT_TRUE(growth.grew_chain) << "chain never grew";

    auto bytes = store.Get(growth.page_id);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_EQ(PageView(bytes.value().bytes()).min_key(), id);
}

TEST(HeapChainTest, GrowthStampsTheOwnerOidOnTheNewPage) {
    // page.md section 2a: every page a chain grows carries the relation's
    // oid the caller passed, readable straight off the common header.
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    ChainInsertResult growth{};
    for (std::uint64_t id = 1; id <= 200; ++id) {
        auto r = ChainInsert(store, head, id, MakeTuple(id, 1016), /*trx_id=*/1,
                             /*owner_oid=*/4001);
        ASSERT_TRUE(r.ok()) << r.status().message();
        if (r.value().grew_chain) {
            growth = r.value();
            break;
        }
    }
    ASSERT_TRUE(growth.grew_chain) << "chain never grew";

    auto bytes = store.Get(growth.page_id);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()), 4001u);
}

TEST(HeapChainTest, TheOldTailIsLinkedToTheNewOneAndKeepsItsMinKey) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    auto placed = FillChain(store, head, 40, /*filler=*/1016);
    ASSERT_EQ(placed.size(), 40u);

    auto head_bytes = store.Get(head);
    ASSERT_TRUE(head_bytes.ok()) << head_bytes.status().message();
    PageView head_page(head_bytes.value().bytes());

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

    auto dup = ChainInsert(store, head, 5, MakeTuple(5, 56), /*trx_id=*/1, /*owner_oid=*/0);
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
    const std::uint64_t tail_min_key = PageView(tail_bytes.value().bytes()).min_key();
    ASSERT_GT(tail_min_key, 1u) << "test needs a grown chain";

    // A sequence that went backwards. Writing this tuple would either
    // violate invariant 3 or hide a duplicate on an earlier page; both are
    // worse than refusing.
    auto backwards = ChainInsert(store, head, 1, MakeTuple(1, 1016), /*trx_id=*/1, /*owner_oid=*/0);
    EXPECT_FALSE(backwards.ok());
    EXPECT_EQ(backwards.status().code(), StatusCode::kOutOfRange);
}

TEST(HeapChainTest, APayloadWhoseKeystoneDisagreesWithTheIdIsRefused) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);

    auto mismatched = ChainInsert(store, head, 7, MakeTuple(9, 56), /*trx_id=*/1, /*owner_oid=*/0);
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
    auto& [second_id, second_bytes_ref] = second.value();
    const std::span<std::byte, kPageSize> second_bytes = second_bytes_ref.bytes();
    auto second_page = PageView::CreateEmpty(second_bytes, 0);
    ASSERT_TRUE(second_page.ok()) << second_page.status().message();

    auto head_bytes = store.Get(head);
    ASSERT_TRUE(head_bytes.ok()) << head_bytes.status().message();
    PageView(head_bytes.value().bytes()).set_next_page_id(second_id);
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
    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
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
// The property in all three: kStop ends the walk with **Status::OK()**, so
// a caller can tell "I found what I wanted" from "something broke" without
// reading a message. The test above is the other half of that pair - same
// stopping point, opposite verdict.

// Counts the slots on the first page of a filled chain. The boundary test
// needs it, and deriving it from the placements beats hard-coding a number
// that changes whenever the header or a filler size does.
std::size_t SlotsOnFirstPage(const std::vector<ChainInsertResult>& placed) {
    std::size_t n = 0;
    for (const auto& p : placed) {
        if (p.page_id == placed.front().page_id) ++n;
    }
    return n;
}

TEST(HeapChainTest, AVisitorCanStopMidPageAndTheWalkSucceeds) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    const auto placed = FillChain(store, head, 40, /*filler=*/1016);
    ASSERT_GT(SlotsOnFirstPage(placed), 3u) << "test needs the stop to land mid-page";

    int visits = 0;
    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++visits;
            return visits == 3 ? storage::VisitControl::kStop : storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(visits, 3) << "the walk continued past the stop";
}

TEST(HeapChainTest, StoppingOnAPagesLastSlotNeverFetchesTheNextPage) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    const auto placed = FillChain(store, head, 40, /*filler=*/1016);

    const PageId first_page = placed.front().page_id;
    const std::size_t on_first = SlotsOnFirstPage(placed);
    ASSERT_LT(on_first, placed.size()) << "test needs a multi-page chain to mean anything";

    // The boundary case the loop is easiest to get wrong at: the stop
    // happens on the last slot of a page, where the very next thing the
    // walk would otherwise do is follow next_page_id. If it checks the
    // outcome after that hop instead of before, this still passes the
    // count check but has already paid for a page fetch - so the assertion
    // is on the pages *seen*, not just on the visit count.
    std::size_t visits = 0;
    std::vector<PageId> pages_seen;
    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId page_id, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            if (pages_seen.empty() || pages_seen.back() != page_id) pages_seen.push_back(page_id);
            ++visits;
            return visits == on_first ? storage::VisitControl::kStop
                                      : storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(visits, on_first);
    ASSERT_EQ(pages_seen.size(), 1u) << "the walk crossed into the next page after kStop";
    EXPECT_EQ(pages_seen.front(), first_page);
}

TEST(HeapChainTest, SteppingOnePageAtATimeVisitsExactlyWhatTheWholeChainWalkDoes) {
    // ChainVisitOnePage is the page-boundary form the executor's walk loop
    // owns (workplan-crosscore.md P4d-3). The contract: stepping the chain
    // by returned next-page ids visits the same (page, slot) sequence as
    // ChainVisit, and the last page answers kInvalidPageId.
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 40, /*filler=*/1016);

    std::vector<std::pair<PageId, std::uint16_t>> whole;
    Status s = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId page_id, PageView&, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            whole.emplace_back(page_id, slot);
            return storage::VisitControl::kContinue;
        });
    ASSERT_TRUE(s.ok()) << s.message();

    std::vector<std::pair<PageId, std::uint16_t>> stepped;
    std::size_t boundaries = 0;
    PageId cur = head;
    while (cur != kInvalidPageId) {
        auto next = ChainVisitOnePage(
            store, cur, storage::PageAccess::kRead,
            [&](PageId page_id, PageView&, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                stepped.emplace_back(page_id, slot);
                return storage::VisitControl::kContinue;
            });
        ASSERT_TRUE(next.ok()) << next.status().message();
        cur = next.value();
        ++boundaries;
    }
    EXPECT_EQ(stepped, whole);
    EXPECT_GT(boundaries, 1u) << "test needs a multi-page chain to exercise a real boundary";
}

TEST(HeapChainTest, AStopInsideOnePageEndsTheSteppedWalk) {
    // kStop and end-of-chain deliberately share kInvalidPageId: either way
    // the walk is over, successfully, and the caller need not tell them
    // apart - the same shape ChainVisit gives its callers.
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 40, /*filler=*/1016);

    auto next = ChainVisitOnePage(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            return storage::VisitControl::kStop;
        });
    ASSERT_TRUE(next.ok()) << next.status().message();
    EXPECT_EQ(next.value(), kInvalidPageId);
}

TEST(HeapChainTest, StoppingOnTheLastSlotOfTheChainIsIndistinguishableFromFinishing) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    const auto placed = FillChain(store, head, 40, /*filler=*/1016);

    // Stopping on the final slot and running off the end must produce the
    // same verdict - otherwise `Exists` over a matching last row would
    // report differently from `Exists` over no match at all.
    std::size_t visits = 0;
    Status stopped = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++visits;
            return visits == placed.size() ? storage::VisitControl::kStop
                                           : storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(stopped.ok()) << stopped.message();
    EXPECT_EQ(visits, placed.size());

    std::size_t ran_out = 0;
    Status finished = ChainVisit(
        store, head, storage::PageAccess::kRead,
        [&](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
            ++ran_out;
            return storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(finished.ok()) << finished.message();
    EXPECT_EQ(ran_out, visits);
}

TEST(HeapChainTest, AVisitorReturningAnOkStatusInsteadOfContinueIsRefused) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 10, /*filler=*/56);

    // `return Status::OK();` is what every visitor said before the walk
    // became stoppable, so it is the mistake a port makes. It builds a
    // StatusOr that reports ok() with no value in it, and reading that
    // value would be undefined - so the walk refuses it by name instead.
    Status s = ChainVisit(store, head, storage::PageAccess::kRead,
                          [](PageId, PageView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
                              return Status::OK();
                          });
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("VisitControl"), std::string::npos) << s.message();
}

// ---- The tail hint (bench/results-bulk-insert.md's fix) -------------------
//
// The claim the hint rests on: a former chain page is always a valid walk
// start, because next_page_id is write-once and a page never leaves its
// chain - so a hint can be *behind* the tail, never wrong, and a damaged
// one costs a retried walk from the head, never an answer.

TEST(HeapChainTest, AHintedInsertMatchesTheUnhintedChainAndTracksTheTail) {
    storage::InMemoryPageStore hinted_store(128);
    storage::InMemoryPageStore plain_store(128);
    const PageId hinted_head = MakeHead(hinted_store);
    const PageId plain_head = MakeHead(plain_store);

    PageId hint = kInvalidPageId;
    for (std::uint64_t id = 1; id <= 30; ++id) {
        auto hinted =
            ChainInsert(hinted_store, hinted_head, id, MakeTuple(id, 1016), /*trx_id=*/1, /*owner_oid=*/0, &hint);
        auto plain = ChainInsert(plain_store, plain_head, id, MakeTuple(id, 1016), /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(hinted.ok()) << hinted.status().message();
        ASSERT_TRUE(plain.ok()) << plain.status().message();

        // Identical placement decision, insert for insert...
        EXPECT_EQ(hinted.value().slot, plain.value().slot);
        EXPECT_EQ(hinted.value().grew_chain, plain.value().grew_chain);

        // ...and the hint is the real tail after every one.
        auto tail = ChainTail(hinted_store, hinted_head);
        ASSERT_TRUE(tail.ok());
        EXPECT_EQ(hint, tail.value()) << "id " << id;
        EXPECT_EQ(hint, hinted.value().page_id);
    }

    auto len = ChainLength(hinted_store, hinted_head);
    ASSERT_TRUE(len.ok());
    EXPECT_GT(len.value(), 2u) << "the fixture must span pages to prove anything";
}

TEST(HeapChainTest, AStaleHintIsBehindNeverWrong) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 20, 1016);  // several pages

    // The stalest possible hint: the head itself. The walk starts there,
    // reaches the true tail, and the insert lands exactly where an
    // unhinted one would.
    PageId hint = head;
    auto r = ChainInsert(store, head, 21, MakeTuple(21, 1016), /*trx_id=*/1, /*owner_oid=*/0, &hint);
    ASSERT_TRUE(r.ok()) << r.status().message();

    auto tail = ChainTail(store, head);
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(r.value().page_id, tail.value());
    EXPECT_EQ(hint, tail.value());
}

TEST(HeapChainTest, ADamagedHintFallsBackToTheHeadAndHeals) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    FillChain(store, head, 20, 1016);

    // A page id the store cannot resolve: the hinted walk fails, the
    // head walk answers, and the write-back repairs the hint.
    PageId hint = 120;  // never allocated
    auto r = ChainInsert(store, head, 21, MakeTuple(21, 1016), /*trx_id=*/1, /*owner_oid=*/0, &hint);
    ASSERT_TRUE(r.ok()) << r.status().message();

    auto tail = ChainTail(store, head);
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(hint, tail.value());
}

// ---- ChainAppendBatch (docs/inflight/in-progress/workplan-t3.md TS02) --------------------------

// The strongest claim available: the batch fill and the sequential row
// loop produce byte-identical pages. Same payloads, two fresh stores,
// every touched page compared whole.
TEST(HeapChainTest, ABatchFillEqualsTheSequentialChainByteForByte) {
    storage::InMemoryPageStore batch_store(128);
    storage::InMemoryPageStore seq_store(128);
    const PageId batch_head = MakeHead(batch_store);
    const PageId seq_head = MakeHead(seq_store);
    ASSERT_EQ(batch_head, seq_head);

    constexpr std::uint64_t kRows = 25;
    std::vector<std::vector<std::byte>> payloads;
    for (std::uint64_t id = 1; id <= kRows; ++id) {
        payloads.push_back(MakeTuple(id, 1016));
    }

    auto batched = ChainAppendBatch(batch_store, batch_head, /*first_id=*/1, payloads,
                                    /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_TRUE(batched.ok()) << batched.status().message();
    ASSERT_EQ(batched.value().rows.size(), kRows);

    for (std::uint64_t id = 1; id <= kRows; ++id) {
        auto r = ChainInsert(seq_store, seq_head, id, payloads[id - 1], /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(r.ok()) << r.status().message();
        // Placement decisions agree row for row.
        EXPECT_EQ(batched.value().rows[id - 1].page_id, r.value().page_id) << "id " << id;
        EXPECT_EQ(batched.value().rows[id - 1].slot, r.value().slot) << "id " << id;
    }

    ASSERT_GT(batched.value().pages.size(), 2u)
        << "the fixture must span pages to prove anything";
    for (const BatchTouchedPage& page : batched.value().pages) {
        auto a = batch_store.Get(page.page_id);
        auto b = seq_store.Get(page.page_id);
        ASSERT_TRUE(a.ok());
        ASSERT_TRUE(b.ok());
        EXPECT_TRUE(std::equal(a.value().bytes().begin(), a.value().bytes().end(), b.value().bytes().begin()))
            << "page " << page.page_id << " diverged";
    }
}

TEST(HeapChainTest, ABatchWhosePayloadIdsDisagreeIsCorruption) {
    storage::InMemoryPageStore store(128);
    const PageId head = MakeHead(store);
    std::vector<std::vector<std::byte>> payloads;
    payloads.push_back(MakeTuple(1, 56));
    payloads.push_back(MakeTuple(9, 56));  // expected 2
    auto r = ChainAppendBatch(store, head, /*first_id=*/1, payloads, /*trx_id=*/1, /*owner_oid=*/0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::heap
