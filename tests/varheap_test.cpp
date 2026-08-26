#include "kds/storage/varheap.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"

// The var-heap page class and chain (docs/rules/rule-fixed-length-tuple.md
// section 5).
//
// The properties under test are the ones the design leans on, not the API
// surface: **a value once written is never moved and never rewritten**, so
// an old pointer keeps resolving to the same bytes; the class is an
// ordinary headered, checksummable page rather than an advisory one; and a
// pointer resolves in one fetch, never a walk.

namespace kds::varheap {
namespace {

using PageBuf = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> AsSpan(PageBuf& buf) { return std::span<std::byte, kPageSize>(buf); }
std::span<const std::byte, kPageSize> AsConst(const PageBuf& buf) {
    return std::span<const std::byte, kPageSize>(buf);
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out;
    for (char c : text) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

std::string TextOf(std::span<const std::byte> bytes) {
    std::string out;
    for (std::byte b : bytes) out.push_back(static_cast<char>(b));
    return out;
}

// ---- The pointer ---------------------------------------------------------

TEST(VarHeapPtrTest, RoundTripsThroughTheCellWord) {
    for (PageId page : {PageId{0}, PageId{1}, PageId{128}, PageId{0x7FFFFFFFu}}) {
        for (std::uint16_t slot : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0xFFFF}}) {
            const VarHeapPtr ptr{page, slot};
            EXPECT_EQ(DecodePtr(EncodePtr(ptr)), ptr) << page << "/" << slot;
        }
    }
}

TEST(VarHeapPtrTest, TheLowSixteenBitsAreReservedAndWrittenZero) {
    // The cell has room for a u64 and the pointer needs 48 bits. The spare
    // 16 are written 0 so a future use of them can tell "unset" from "set".
    EXPECT_EQ(EncodePtr(VarHeapPtr{0xFFFFFFFFu, 0xFFFF}) & 0xFFFFu, 0u);
}

// ---- One page ------------------------------------------------------------

TEST(VarHeapPageTest, FormatProducesAnEmptyHeaderedPage) {
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());

    // An ordinary headered page class, not an advisory one: it validates,
    // it checksums, and it carries a page_lsn. Losing a var-heap value
    // loses a committed value, not a hint.
    EXPECT_TRUE(storage::ValidatePageHeader(AsConst(buf), PageType::kVarHeap).ok());
    storage::StampPageChecksum(AsSpan(buf));
    EXPECT_TRUE(storage::VerifyPageChecksum(AsConst(buf)).ok());

    EXPECT_EQ(PageSlotCount(AsConst(buf)), 0);
    EXPECT_EQ(PageNextPageId(AsConst(buf)), kInvalidPageId);
}

TEST(VarHeapPageTest, ValuesRoundTripInSlotOrder) {
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());

    const std::vector<std::string> values = {"alpha", "", "gamma with spaces",
                                             std::string(2000, 'z')};
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto slot = PageAppend(AsSpan(buf), Bytes(values[i]));
        ASSERT_TRUE(slot.ok()) << slot.status().message();
        EXPECT_EQ(slot.value(), i);  // slots are handed out in order
    }

    for (std::size_t i = 0; i < values.size(); ++i) {
        auto read = PageRead(AsConst(buf), static_cast<std::uint16_t>(i));
        ASSERT_TRUE(read.ok()) << read.status().message();
        EXPECT_EQ(TextOf(read.value()), values[i]);
    }
}

TEST(VarHeapPageTest, AnEarlierValueIsUnchangedByEveryLaterAppend) {
    // The property the whole class exists for: values are immutable per
    // version, so a pointer taken before a hundred more appends still
    // resolves to the same bytes. MVCC correctness rides on exactly this.
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());

    auto first = PageAppend(AsSpan(buf), Bytes("the original"));
    ASSERT_TRUE(first.ok());

    for (int i = 0; i < 100; ++i) {
        auto more = PageAppend(AsSpan(buf), Bytes("filler " + std::to_string(i)));
        ASSERT_TRUE(more.ok());
    }

    auto read = PageRead(AsConst(buf), first.value());
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(TextOf(read.value()), "the original");
}

TEST(VarHeapPageTest, AFullPageReportsOutOfSpaceRatherThanOverwriting) {
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());

    auto filled = PageAppend(AsSpan(buf), std::vector<std::byte>(kMaxValueSize, std::byte{'x'}));
    ASSERT_TRUE(filled.ok()) << filled.status().message();

    // OutOfSpace is the signal the chain grows on, so it has to be
    // distinguishable from a real failure.
    auto overflow = PageAppend(AsSpan(buf), Bytes("one more"));
    ASSERT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.status().code(), StatusCode::kOutOfSpace);
}

TEST(VarHeapPageTest, AValueLargerThanAPageIsUnsupportedNotTruncated) {
    // The spilled-value size cap is an open decision, and this layer
    // refuses rather than inventing a multi-page representation to answer
    // it (varheap.hpp's kMaxValueSize).
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());

    auto too_big = PageAppend(AsSpan(buf), std::vector<std::byte>(kMaxValueSize + 1, std::byte{0}));
    ASSERT_FALSE(too_big.ok());
    EXPECT_EQ(too_big.status().code(), StatusCode::kUnsupported);
}

TEST(VarHeapPageTest, AnOutOfRangeSlotIsCorruption) {
    PageBuf buf{};
    ASSERT_TRUE(FormatPage(AsSpan(buf)).ok());
    ASSERT_TRUE(PageAppend(AsSpan(buf), Bytes("only one")).ok());

    EXPECT_EQ(PageRead(AsConst(buf), 1).status().code(), StatusCode::kCorruption);
    EXPECT_EQ(PageRead(AsConst(buf), 9999).status().code(), StatusCode::kCorruption);
}

// ---- The chain -----------------------------------------------------------

class VarHeapChainTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{128};
};

TEST_F(VarHeapChainTest, AppendAndFetchRoundTrip) {
    auto root = CreateChain(store_, /*owner_oid=*/0);
    ASSERT_TRUE(root.ok()) << root.status().message();

    auto ptr = ChainAppend(store_, root.value(), Bytes("a spilled value"), /*owner_oid=*/0);
    ASSERT_TRUE(ptr.ok()) << ptr.status().message();

    storage::PageRef fetch_pin;
    auto fetched = Fetch(store_, ptr.value().ptr, fetch_pin);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    EXPECT_EQ(TextOf(fetched.value()), "a spilled value");

    // The root had room, so nothing structural happened - and saying so is
    // what keeps a caller from logging a PAGE_INIT for a page it did not
    // create.
    EXPECT_FALSE(ptr.value().grew());
    EXPECT_EQ(ptr.value().created_page_id, kInvalidPageId);
    EXPECT_EQ(ptr.value().linked_page_id, kInvalidPageId);
}

TEST_F(VarHeapChainTest, GrowthReportsThePageItCreatedAndTheTailItLinked) {
    // The report the WAL needs and did not have: a VARHEAP_APPEND describes
    // neither the page's creation nor the link that reaches it, so recovery
    // met an append naming a page nothing created and refused the mount
    // (docs/inflight/known-gaps.md's var-heap entry).
    auto root = CreateChain(store_, /*owner_oid=*/0);
    ASSERT_TRUE(root.ok());

    PageId last_tail = root.value();
    int growths = 0;
    for (int i = 0; i < 40; ++i) {
        auto appended =
            ChainAppend(store_, root.value(), std::vector<std::byte>(1000, std::byte{'z'}), 0);
        ASSERT_TRUE(appended.ok()) << appended.status().message();

        if (!appended.value().grew()) {
            // No growth means no structural record is owed, and the value
            // landed in the page that was already the tail.
            EXPECT_EQ(appended.value().linked_page_id, kInvalidPageId);
            EXPECT_EQ(appended.value().ptr.page_id, last_tail);
            continue;
        }
        ++growths;
        // The created page is the one the value landed in, and the linked
        // page is the tail it came after - which is exactly the pair
        // PAGE_INIT and the full page image are logged for.
        EXPECT_EQ(appended.value().created_page_id, appended.value().ptr.page_id);
        EXPECT_EQ(appended.value().linked_page_id, last_tail);
        EXPECT_EQ(PageNextPageId(store_.GetForRead(last_tail).value().bytes()),
                  appended.value().created_page_id)
            << "the reported link is not the link the page carries";
        last_tail = appended.value().created_page_id;
    }
    EXPECT_GT(growths, 0) << "the chain never grew; the test proves nothing";
}

TEST_F(VarHeapChainTest, GrowsByTailAppendAndKeepsEveryEarlierPointerValid) {
    auto root = CreateChain(store_, /*owner_oid=*/0);
    ASSERT_TRUE(root.ok());

    // Values sized so several pages are needed.
    std::vector<VarHeapPtr> pointers;
    std::vector<std::string> values;
    for (int i = 0; i < 40; ++i) {
        values.push_back(std::string(1000, static_cast<char>('a' + (i % 26))));
        auto ptr = ChainAppend(store_, root.value(), Bytes(values.back()), 0);
        ASSERT_TRUE(ptr.ok()) << ptr.status().message();
        pointers.push_back(ptr.value().ptr);
    }

    auto length = ChainLength(store_, root.value());
    ASSERT_TRUE(length.ok());
    EXPECT_GT(length.value(), 1u) << "the chain never grew; the test proves nothing";

    // Every pointer taken along the way still resolves, and to the same
    // bytes. Nothing was moved to make room - that is the design.
    for (std::size_t i = 0; i < pointers.size(); ++i) {
        storage::PageRef fetch_pin;
        auto fetched = Fetch(store_, pointers[i], fetch_pin);
        ASSERT_TRUE(fetched.ok()) << "pointer " << i << ": " << fetched.status().message();
        EXPECT_EQ(TextOf(fetched.value()), values[i]) << "pointer " << i;
    }
}

TEST_F(VarHeapChainTest, TheRootNeverMovesWhenTheChainGrows) {
    // Why sys.tables can cache varheap_page_id: growth edits the tail's
    // link, never the root, so the root is fixed by DDL.
    auto root = CreateChain(store_, /*owner_oid=*/0);
    ASSERT_TRUE(root.ok());

    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(ChainAppend(store_, root.value(), std::vector<std::byte>(1000, std::byte{'q'}), 0)
                        .ok());
    }
    auto length = ChainLength(store_, root.value());
    ASSERT_TRUE(length.ok());
    EXPECT_GT(length.value(), 1u);

    // Still a var-heap page, still the head of the chain.
    auto page = store_.GetForRead(root.value());
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(storage::ValidatePageHeader(page.value().bytes(), PageType::kVarHeap).ok());
}

TEST_F(VarHeapChainTest, FetchingThroughAPointerAtANonVarHeapPageIsRefused) {
    // A pointer is only as good as the tuple it came from. Reading a heap
    // page's bytes as values would return garbage where this returns a
    // failure.
    auto created = store_.CreateNew();
    ASSERT_TRUE(created.ok());
    storage::FormatPage(created.value().second.bytes(), PageType::kHeap);

    storage::PageRef fetch_pin;
    auto fetched = Fetch(store_, VarHeapPtr{created.value().first, 0}, fetch_pin);
    ASSERT_FALSE(fetched.ok());
    EXPECT_EQ(fetched.status().code(), StatusCode::kCorruption);
}

TEST_F(VarHeapChainTest, AppendingWithNoChainIsRefused) {
    auto ptr = ChainAppend(store_, kInvalidPageId, Bytes("nowhere to go"), 0);
    ASSERT_FALSE(ptr.ok());
    EXPECT_EQ(ptr.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(VarHeapChainTest, ChainLengthOfNoChainIsZero) {
    auto length = ChainLength(store_, kInvalidPageId);
    ASSERT_TRUE(length.ok());
    EXPECT_EQ(length.value(), 0u);
}

}  // namespace
}  // namespace kds::varheap
