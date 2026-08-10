#include "kds/wal/redo.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/undo_page.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/stream.hpp"

// RC03 - the redo phase (docs/workplan-wal-recovery.md).
//
// Two properties carry this task and both are about *re-running*, because
// recovery must survive being interrupted by the crash that follows it:
//
//   - a record at or below its page's page_lsn is skipped (RV5);
//   - replaying the whole range twice leaves the pages byte-identical.
//
// The primitives underneath get their own tests, because "place at the
// slot the record names" is the property every durable reference to a
// tuple depends on - an undo record names (page, slot), so a tuple that
// came back elsewhere would leave the chain pointing at the wrong row.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kPage = server::kFirstUserPageId;

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

class RedoTest : public ::testing::Test {
protected:
    MemoryLogDevice device_{kSegmentSize};
    storage::InMemoryPageStore store_{server::kFirstUserPageId};

    // Appends PAGE_INIT for a heap page plus `n` tuple inserts, and returns
    // the analysis of the resulting stream.
    void WriteHeapStream(int n, unsigned char fill = 0xA1) {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, 1, kPage}, init).ok());

        for (int i = 0; i < n; ++i) {
            const auto payload = Bytes(24, static_cast<unsigned char>(fill + i));
            std::vector<std::byte> buf(kHeapWriteFixedSize + payload.size(), std::byte{0});
            const HeapWritePayload hw{/*trx_id=*/7, /*undo_ptr=*/0,
                                      static_cast<std::uint16_t>(i),
                                      static_cast<std::uint16_t>(payload.size())};
            auto n_enc = EncodeHeapWrite(buf, hw, payload);
            ASSERT_TRUE(n_enc.ok()) << n_enc.status().message();
            ASSERT_TRUE(s.value()
                            ->Append({RecordType::kHeapInsert, 7, kPage},
                                     std::span(buf).first(n_enc.value()))
                            .ok());
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    AnalysisResult Analyzed() {
        auto a = Analyze(device_, 0, AnalysisStart{});
        EXPECT_TRUE(a.ok()) << a.status().message();
        return a.ok() ? a.value() : AnalysisResult{};
    }

    std::vector<std::byte> PageBytes(PageId id) {
        auto p = store_.Get(id);
        EXPECT_TRUE(p.ok()) << p.status().message();
        if (!p.ok()) return {};
        return std::vector<std::byte>(p.value().begin(), p.value().end());
    }
};

// ---- The phase -----------------------------------------------------------

TEST_F(RedoTest, ReplaysAHeapStreamOntoAStoreThatNeverSawIt) {
    WriteHeapStream(3);
    auto r = Redo(device_, 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 4u);       // one PAGE_INIT + three inserts
    EXPECT_EQ(r.value().pages_created, 1u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value());
    EXPECT_EQ(view.slot_count(), 3u);
    auto tuple = view.ReadTuple(1);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();
    EXPECT_EQ(tuple.value().trx_id, 7u);
    EXPECT_EQ(std::vector<std::byte>(tuple.value().payload.begin(), tuple.value().payload.end()),
              Bytes(24, 0xA2));
}

TEST_F(RedoTest, ReplayingTwiceIsAByteForByteNoOp) {
    // The property the phase exists to keep: recovery can be interrupted by
    // another crash and re-run, so applying the same range twice must not
    // move a byte.
    WriteHeapStream(4);
    const AnalysisResult analysis = Analyzed();

    ASSERT_TRUE(Redo(device_, 0, store_, analysis).ok());
    const std::vector<std::byte> after_first = PageBytes(kPage);

    auto second = Redo(device_, 0, store_, analysis);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(PageBytes(kPage), after_first);

    // And the second pass did the work of skipping rather than of applying.
    EXPECT_EQ(second.value().applied, 0u);
    EXPECT_EQ(second.value().skipped_by_lsn, 5u);
}

TEST_F(RedoTest, ARecordAtOrBelowThePagesLsnIsSkipped) {
    WriteHeapStream(2);
    const AnalysisResult analysis = Analyzed();

    // Stamp the page past the whole stream: nothing may be applied.
    auto created = store_.CreateAt(kPage);
    ASSERT_TRUE(created.ok());
    storage::FormatPage(created.value(), PageType::kHeap);
    storage::SetPageLsn(created.value(), analysis.end_lsn + 1);

    auto r = Redo(device_, 0, store_, analysis);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().skipped_by_lsn, 3u);
}

TEST_F(RedoTest, AFullPageImageRestoresTheWholePage) {
    std::vector<std::byte> image(kPageSize, std::byte{0});
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        // A page built by hand, logged whole.
        auto view = heap::PageView::CreateEmpty(
            std::span<std::byte, kPageSize>(image.data(), kPageSize), /*min_key=*/5);
        ASSERT_TRUE(view.ok());
        ASSERT_TRUE(view.value().InsertTuple(Bytes(16, 0xEE), 3, 0).ok());

        std::vector<std::byte> buf(kFullPageImagePayloadSize, std::byte{0});
        ASSERT_TRUE(EncodeFullPageImage(
                        buf, std::span<const std::byte, kPageSize>(image.data(), kPageSize))
                        .ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kFullPageImage, 3, kPage}, buf).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    auto r = Redo(device_, 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().page_images, 1u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value());
    EXPECT_EQ(view.min_key(), 5u);
    EXPECT_EQ(view.slot_count(), 1u);
}

TEST_F(RedoTest, TransactionAndCheckpointRecordsChangeNoPage) {
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Redo(device_, 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().no_page, 2u);
}

// ---- The UNDO_WRITE gap, asserted rather than worked around -------------

TEST_F(RedoTest, RedoOfUndoWriteRebuildsARecordThatNamesItsTuple) {
    // The case that could not be written before 2026-08-10: the record now
    // carries the undo record's tail, so replay restores target_page_id,
    // target_slot and type - the fields that say *which tuple* the image
    // belongs to, and without which RC05's undo would roll back the wrong
    // row rather than fail.
    const auto image = Bytes(8, 0x5A);
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload f{0, static_cast<std::uint8_t>(PageType::kUndo), {0, 0, 0}};
        ASSERT_TRUE(EncodePageInit(init, f).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, 1, kPage}, init).ok());

        txn::UndoRecordFields rec{};
        rec.target_page_id = 777;
        rec.target_slot = 3;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kOverwrite);
        std::vector<std::byte> tail(txn::UndoRecordTailSize(image.size()));
        ASSERT_TRUE(txn::EncodeUndoRecordTail(tail, rec, image).ok());

        std::vector<std::byte> buf(kUndoWriteFixedSize + tail.size(), std::byte{0});
        const UndoWritePayload p{/*prior_trx_id=*/2, /*prior_undo_ptr=*/0,
                                 static_cast<std::uint16_t>(txn::kUndoRecordsOffset),
                                 static_cast<std::uint16_t>(tail.size())};
        auto n = EncodeUndoWrite(buf, p, tail);
        ASSERT_TRUE(n.ok()) << n.status().message();
        ASSERT_TRUE(
            s.value()->Append({RecordType::kUndoWrite, 1, kPage}, std::span(buf).first(n.value()))
                .ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    auto r = Redo(device_, 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    auto back = txn::UndoPageRead(std::span<const std::byte, kPageSize>(page.value()),
                                  static_cast<std::uint16_t>(txn::kUndoRecordsOffset));
    ASSERT_TRUE(back.ok()) << back.status().message();
    EXPECT_EQ(back.value().fields.target_page_id, 777u);
    EXPECT_EQ(back.value().fields.target_slot, 3u);
    EXPECT_EQ(back.value().fields.prior_trx_id, 2u);
    EXPECT_EQ(std::vector<std::byte>(back.value().image.begin(), back.value().image.end()), image);
}

// ---- The heap primitive --------------------------------------------------

class RedoWriteTupleTest : public ::testing::Test {
protected:
    std::vector<std::byte> buf_ = std::vector<std::byte>(kPageSize, std::byte{0});
    std::span<std::byte, kPageSize> page() {
        return std::span<std::byte, kPageSize>(buf_.data(), kPageSize);
    }
    void Format() {
        auto v = heap::PageView::CreateEmpty(page(), 1);
        ASSERT_TRUE(v.ok()) << v.status().message();
    }
};

TEST_F(RedoWriteTupleTest, AppendsWhenTheSlotIsTheNextOne) {
    Format();
    heap::PageView view(page());
    ASSERT_TRUE(view.RedoWriteTuple(0, Bytes(16, 1), 9, 0).ok());
    ASSERT_TRUE(view.RedoWriteTuple(1, Bytes(16, 2), 9, 0).ok());
    EXPECT_EQ(view.slot_count(), 2u);
    auto t = view.ReadTuple(1);
    ASSERT_TRUE(t.ok());
    EXPECT_EQ(std::vector<std::byte>(t.value().payload.begin(), t.value().payload.end()),
              Bytes(16, 2));
}

TEST_F(RedoWriteTupleTest, ReWritingAnExistingSlotIsAByteForByteNoOp) {
    Format();
    heap::PageView view(page());
    ASSERT_TRUE(view.RedoWriteTuple(0, Bytes(16, 1), 9, 0).ok());
    const std::vector<std::byte> after_first = buf_;

    ASSERT_TRUE(view.RedoWriteTuple(0, Bytes(16, 1), 9, 0).ok());
    EXPECT_EQ(buf_, after_first);
    EXPECT_EQ(view.slot_count(), 1u) << "a re-application allocated a second slot";
}

TEST_F(RedoWriteTupleTest, ASlotPastTheDirectoryIsCorruption) {
    Format();
    heap::PageView view(page());
    auto s = view.RedoWriteTuple(3, Bytes(16, 1), 9, 0);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

TEST_F(RedoWriteTupleTest, ARetiredSlotIsCorruptionRatherThanAResurrection) {
    Format();
    heap::PageView view(page());
    ASSERT_TRUE(view.RedoWriteTuple(0, Bytes(16, 1), 9, 0).ok());
    ASSERT_TRUE(view.RetireSlot(0).ok());
    auto s = view.RedoWriteTuple(0, Bytes(16, 1), 9, 0);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

// ---- The var-heap primitive ---------------------------------------------

class VarHeapWriteAtTest : public ::testing::Test {
protected:
    std::vector<std::byte> buf_ = std::vector<std::byte>(kPageSize, std::byte{0});
    std::span<std::byte, kPageSize> page() {
        return std::span<std::byte, kPageSize>(buf_.data(), kPageSize);
    }
    void SetUp() override { ASSERT_TRUE(varheap::FormatPage(page()).ok()); }
};

TEST_F(VarHeapWriteAtTest, AppendsAtTheNamedSlotAndIsIdempotent) {
    ASSERT_TRUE(varheap::PageWriteAt(page(), 0, Bytes(32, 7)).ok());
    const std::vector<std::byte> after_first = buf_;
    ASSERT_TRUE(varheap::PageWriteAt(page(), 0, Bytes(32, 7)).ok());
    EXPECT_EQ(buf_, after_first);

    auto v = varheap::PageRead(page(), 0);
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value().size(), 32u);
}

TEST_F(VarHeapWriteAtTest, ARewriteOfADifferentLengthIsCorruption) {
    // Invariant 14: a var-heap value is immutable per version, so the only
    // legal second write is the same bytes.
    ASSERT_TRUE(varheap::PageWriteAt(page(), 0, Bytes(32, 7)).ok());
    auto s = varheap::PageWriteAt(page(), 0, Bytes(48, 7));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

TEST_F(VarHeapWriteAtTest, ASlotPastTheEndIsCorruption) {
    auto s = varheap::PageWriteAt(page(), 4, Bytes(8, 1));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

}  // namespace
}  // namespace kds::wal
