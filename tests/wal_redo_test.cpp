#include "kds/wal/redo.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
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

constexpr std::uint64_t kSegmentSize = kMinSegmentSize;  // the stream's floor (`stream.hpp`)
constexpr PageId kPage = server::kFirstUserPageId;

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

class RedoTest : public ::testing::Test {
protected:
    std::unique_ptr<MemoryLogDevice> device_ =
        std::move(MemoryLogDevice::Create(kSegmentSize).value());
    storage::InMemoryPageStore store_{server::kFirstUserPageId};

    // Appends one HEAP_INSERT at `slot` and returns its LSN (0 on failure).
    Lsn AppendHeapInsert(WalStream& w, std::uint16_t slot, unsigned char fill) {
        const auto payload = Bytes(24, fill);
        std::vector<std::byte> buf(kHeapWriteFixedSize + payload.size(), std::byte{0});
        const HeapWritePayload hw{/*trx_id=*/7, /*undo_ptr=*/0, slot,
                                  static_cast<std::uint16_t>(payload.size())};
        auto n_enc = EncodeHeapWrite(buf, hw, payload);
        EXPECT_TRUE(n_enc.ok()) << n_enc.status().message();
        if (!n_enc.ok()) return 0;
        auto lsn = w.Append({RecordType::kHeapInsert, 7, kPage},
                            std::span(buf).first(n_enc.value()));
        EXPECT_TRUE(lsn.ok()) << lsn.status().message();
        return lsn.ok() ? lsn.value() : 0;
    }

    // Appends PAGE_INIT for a heap page plus `n` tuple inserts, and returns
    // the analysis of the resulting stream.
    void WriteHeapStream(int n, unsigned char fill = 0xA1) {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, 1, kPage}, init).ok());

        for (int i = 0; i < n; ++i) {
            ASSERT_NE(AppendHeapInsert(*s.value(), static_cast<std::uint16_t>(i),
                                       static_cast<unsigned char>(fill + i)),
                      0u);
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    // PAGE_INIT + one insert, logged as `core` would log them - the record
    // naming its core in the envelope's flags byte, which is what a peer
    // appending to the instance's one stream produces.
    void WriteHeapStreamLoggedBy(std::uint32_t core) {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(s.value()
                        ->Append({RecordType::kPageInit, 1, kPage,
                                  static_cast<std::uint8_t>(core)},
                                 init)
                        .ok());
        ASSERT_NE(AppendHeapInsert(*s.value(), /*slot=*/0, 0xA1), 0u);
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    AnalysisResult Analyzed() {
        auto a = Analyze((*device_), 0, AnalysisStart{});
        EXPECT_TRUE(a.ok()) << a.status().message();
        return a.ok() ? a.value() : AnalysisResult{};
    }

    std::vector<std::byte> PageBytes(PageId id) {
        auto p = store_.Get(id);
        EXPECT_TRUE(p.ok()) << p.status().message();
        if (!p.ok()) return {};
        return std::vector<std::byte>(p.value().bytes().begin(), p.value().bytes().end());
    }
};

// ---- The phase -----------------------------------------------------------

TEST_F(RedoTest, PageInitReplaysTheOwnerStamp) {
    // page.md section 2a: the owner rides the PAGE_INIT record, so a page
    // recreated by redo is exactly as attributed as the live path left it.
    auto s = WalStream::Open(device_.get(), 0);
    ASSERT_TRUE(s.ok()) << s.status().message();
    std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
    const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                 {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/4001};
    ASSERT_TRUE(EncodePageInit(init, fields).ok());
    ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, 1, kPage}, init).ok());
    ASSERT_TRUE(s.value()->Sync().ok());

    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();

    const auto bytes = PageBytes(kPage);
    ASSERT_EQ(bytes.size(), kPageSize);
    EXPECT_EQ(storage::GetOwnerOid(
                  std::span<const std::byte, kPageSize>(bytes.data(), kPageSize)),
              4001u);
}

TEST_F(RedoTest, ReplaysAHeapStreamOntoAStoreThatNeverSawIt) {
    WriteHeapStream(3);
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 4u);       // one PAGE_INIT + three inserts
    EXPECT_EQ(r.value().pages_created, 1u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value().bytes());
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

    ASSERT_TRUE(Redo((*device_), 0, store_, analysis).ok());
    const std::vector<std::byte> after_first = PageBytes(kPage);

    auto second = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(PageBytes(kPage), after_first);

    // And the second pass did the work of skipping rather than of applying.
    EXPECT_EQ(second.value().applied, 0u);
    EXPECT_EQ(second.value().skipped_by_lsn, 5u);
}

TEST_F(RedoTest, APurgesSlotRetireReplaysOnceAndTwiceIsANoOp) {
    // BH-R7 (`instructions/v3.0.0/workorder-bh-purge-key.md`): a `PURGE`
    // logs `SLOT_RETIRE` at `kNoTxnId`, no transaction's. Redo applies it
    // with the rollback's own applier - the slot answers NotFound - and a
    // second pass over the same range moves no byte.
    WriteHeapStream(4);
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();
        std::array<std::byte, kSlotRetirePayloadSize> buf{};
        ASSERT_TRUE(EncodeSlotRetire(buf, SlotRetirePayload{/*slot=*/1}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kSlotRetire, kNoTxnId, kPage}, buf).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    const AnalysisResult analysis = Analyzed();

    ASSERT_TRUE(Redo((*device_), 0, store_, analysis).ok());
    {
        auto page = store_.Get(kPage);
        ASSERT_TRUE(page.ok()) << page.status().message();
        heap::PageView view(page.value().bytes());
        EXPECT_EQ(view.ReadTuple(1).status().code(), StatusCode::kNotFound)
            << "the purge's retire was not replayed";
        EXPECT_TRUE(view.ReadTuple(0).ok());
    }
    const std::vector<std::byte> after_first = PageBytes(kPage);
    auto second = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(PageBytes(kPage), after_first);
    EXPECT_EQ(second.value().applied, 0u);
}

TEST_F(RedoTest, ARecordAtOrBelowThePagesLsnIsSkipped) {
    WriteHeapStream(2);
    const AnalysisResult analysis = Analyzed();

    // Stamp the page past the whole stream: nothing may be applied.
    auto created = store_.CreateAt(kPage);
    ASSERT_TRUE(created.ok());
    storage::FormatPage(created.value().bytes(), PageType::kHeap);
    storage::SetPageLsn(created.value().bytes(), analysis.end_lsn + 1);

    auto r = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().skipped_by_lsn, 3u);
}

TEST_F(RedoTest, AFullPageImageRestoresTheWholePage) {
    std::vector<std::byte> image(kPageSize, std::byte{0});
    {
        auto s = WalStream::Open(device_.get(), 0);
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

    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().page_images, 1u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value().bytes());
    EXPECT_EQ(view.min_key(), 5u);
    EXPECT_EQ(view.slot_count(), 1u);
}

TEST_F(RedoTest, TransactionAndCheckpointRecordsChangeNoPage) {
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    // Redone from the head of the log, deliberately. What is under test is
    // the *classification* - that a transaction boundary reaches redo and is
    // counted as touching no page - and an honest analysis of this stream
    // makes redo skip it entirely: no record here dirties a page, so
    // `RedoStartFrom` returns end_lsn and there is correctly nothing to
    // replay (wal.md §11-3). Asking for the classification means arranging
    // for the records to be visited; the redo-start optimization is a
    // separate claim with its own tests.
    AnalysisResult from_head = Analyzed();
    from_head.redo_start_lsn = 0;

    auto r = Redo((*device_), 0, store_, from_head);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().no_page, 2u);
}

TEST_F(RedoTest, ARecordBelowItsPagesRecLsnIsSkippedAndThePageNeverFaulted) {
    // The not-dirty filter, at the arm that survives one stream
    // (AM-S4(d)): a record whose LSN is below its page's recLSN describes
    // state already in the durable image, and must be skipped *before the
    // load* - faulting the page to compare page_lsn would be work on a page
    // recovery has no business touching. The shape it stands for is a page
    // dirtied, written back, and re-dirtied: the checkpoint seeds its
    // recLSN above the flushed records, while the redo start - the minimum
    // over every page - lies below them. Set here rather than scripted,
    // because a second page's low recLSN is the only thing a checkpoint
    // record would add.
    //
    // The store deliberately does not hold the page, which makes any touch
    // loud: an unfiltered redo would CreateAt the page from its PAGE_INIT
    // and apply all three records.
    WriteHeapStream(2);

    AnalysisResult analysis = Analyzed();
    ASSERT_EQ(analysis.dirty_pages.count(kPage), 1u);
    analysis.dirty_pages[kPage] = analysis.end_lsn;
    analysis.redo_start_lsn = 0;

    auto r = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().skipped_not_dirty, 3u) << "init + two inserts";
    EXPECT_FALSE(store_.Get(kPage).ok())
        << "redo must not create or fault a page whose records the image already holds";
}

TEST_F(RedoTest, AnAnchorReplaysItsInitAndItsSlotUpdates) {
    // PW2-1: PAGE_INIT rebuilds only the common header, so the anchor's
    // durable story is PAGE_INIT + ANCHOR_UPDATE - one per slot. A crash
    // losing the unflushed anchor must recover the relation's entry
    // points whole, or the relation loses its roots (RC03's argument one
    // page over).
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        WalStream& w = *s.value();
        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/0,
                                     static_cast<std::uint8_t>(PageType::kAnchor),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/4001};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(w.Append({RecordType::kPageInit, 1, kPage}, init).ok());
        std::array<std::byte, kAnchorUpdatePayloadSize> upd{};
        ASSERT_TRUE(EncodeAnchorUpdate(upd, AnchorUpdatePayload{0, 262}).ok());
        ASSERT_TRUE(w.Append({RecordType::kAnchorUpdate, 1, kPage}, upd).ok());
        ASSERT_TRUE(EncodeAnchorUpdate(upd, AnchorUpdatePayload{9001, 300}).ok());
        ASSERT_TRUE(w.Append({RecordType::kAnchorUpdate, 1, kPage}, upd).ok());
        ASSERT_TRUE(w.Sync().ok());
    }
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 3u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::RawPageType(page.value().bytes()),
              static_cast<std::uint8_t>(PageType::kAnchor));
    EXPECT_EQ(storage::AnchorClusteredRoot(page.value().bytes()), 262u);
    auto idx = storage::AnchorIndexRoot(page.value().bytes(), 9001);
    ASSERT_TRUE(idx.ok()) << idx.status().message();
    EXPECT_EQ(idx.value(), 300u);
}

TEST_F(RedoTest, AnAnchorUpdateAgainstANonAnchorPageIsCorruption) {
    // The 3f07eda review's C2: the applier must refuse a record that is
    // not this page's - a heap page's body bytes read as an entry count
    // are exactly the forged bound the anchor accessors refuse.
    WriteHeapStream(1);
    ASSERT_TRUE(Redo((*device_), 0, store_, Analyzed()).ok());
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kAnchorUpdatePayloadSize> upd{};
        ASSERT_TRUE(EncodeAnchorUpdate(upd, AnchorUpdatePayload{0, 262}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kAnchorUpdate, 1, kPage}, upd).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

// **No stamp** (BP-H5): a page redo creates keeps the `flags` word its
// format wrote, 0, whatever core's byte the record's envelope carries.
TEST_F(RedoTest, ACreatedPageCarriesNoStreamStamp) {
    WriteHeapStreamLoggedBy(/*core=*/2);

    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_GT(r.value().applied, 0u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::ReadPageHeader(page.value().bytes()).flags, 0u)
        << "redo wrote a stream stamp";
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
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload f{0, static_cast<std::uint8_t>(PageType::kUndo), {0, 0, 0},
                                /*reserved2=*/0, /*owner_oid=*/0};
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

    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    auto back = txn::UndoPageRead(std::span<const std::byte, kPageSize>(page.value().bytes()),
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

// ---- BTREE_INSERT and BTREE_SPLIT (BD-S2) ---------------------------------
//
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` BD-R3 E2 and
// BD-R12: a leaf insert replays where its key sorts, shifting the rows after
// it, and refuses a record whose key does not sort there; a split's one
// record replays every page it names.

// A row whose Keystone word carries `tuple_key`.
std::vector<std::byte> Row(std::uint64_t tuple_key) {
    std::vector<std::byte> out(24, std::byte{0x5A});
    auto word = Keystone::Encode(tuple_key, 0, 0);
    EXPECT_TRUE(word.ok());
    const std::uint64_t w = word.ok() ? word.value() : 0;
    std::memcpy(out.data(), &w, sizeof(w));
    return out;
}

void AppendLeafInit(WalStream& w) {
    std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
    const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kBtreeLeaf),
                                 {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
    ASSERT_TRUE(EncodePageInit(init, fields).ok());
    ASSERT_TRUE(w.Append({RecordType::kPageInit, 1, kPage}, init).ok());
}

void AppendBtreeInsert(WalStream& w, std::uint16_t slot, std::uint64_t tuple_key) {
    const std::vector<std::byte> row = Row(tuple_key);
    std::vector<std::byte> buf(kHeapWriteFixedSize + row.size());
    const HeapWritePayload hw{/*trx_id=*/7, /*undo_ptr=*/0, slot,
                              static_cast<std::uint16_t>(row.size())};
    ASSERT_TRUE(EncodeHeapWrite(buf, hw, row).ok());
    ASSERT_TRUE(w.Append({RecordType::kBtreeInsert, 7, kPage}, buf).ok());
}

std::vector<std::uint64_t> LeafKeys(storage::PageStore& store, PageId page_id) {
    auto p = store.Get(page_id);
    EXPECT_TRUE(p.ok()) << p.status().message();
    if (!p.ok()) return {};
    heap::PageView view(p.value().bytes());
    std::vector<std::uint64_t> keys;
    for (std::uint16_t i = 0; i < view.slot_count(); ++i) {
        auto payload = view.PayloadAt(i, view.slot_count());
        if (!payload.ok()) continue;
        auto id = KeystoneIdOfPayload(payload.value());
        EXPECT_TRUE(id.ok());
        keys.push_back(id.ok() ? id.value() : 0);
    }
    return keys;
}

TEST_F(RedoTest, ABtreeInsertReplaysAtItsSlotAndShiftsTheRowsAfterIt) {
    auto s = WalStream::Open(device_.get(), 0);
    ASSERT_TRUE(s.ok()) << s.status().message();
    AppendLeafInit(*s.value());
    AppendBtreeInsert(*s.value(), 0, 10);
    AppendBtreeInsert(*s.value(), 1, 30);
    AppendBtreeInsert(*s.value(), 1, 20);  // between them: 30 moves to slot 2
    ASSERT_TRUE(s.value()->Sync().ok());

    auto redone = Redo(*device_, 0, store_, Analyzed());
    ASSERT_TRUE(redone.ok()) << redone.status().message();
    EXPECT_EQ(LeafKeys(store_, kPage), (std::vector<std::uint64_t>{10, 20, 30}))
        << "a mid-leaf insert replayed over its neighbour (HEAP_INSERT's overwrite arm)";
}

TEST_F(RedoTest, ABtreeInsertWhoseKeyDoesNotSortAtItsSlotIsCorruption) {
    // Strict, with no re-application arm: a record whose key would land out
    // of order is not this page's.
    auto s = WalStream::Open(device_.get(), 0);
    ASSERT_TRUE(s.ok()) << s.status().message();
    AppendLeafInit(*s.value());
    AppendBtreeInsert(*s.value(), 0, 30);
    AppendBtreeInsert(*s.value(), 1, 20);  // after 30: out of order
    ASSERT_TRUE(s.value()->Sync().ok());

    auto redone = Redo(*device_, 0, store_, Analyzed());
    ASSERT_FALSE(redone.ok());
    EXPECT_EQ(redone.status().code(), StatusCode::kCorruption) << redone.status().message();
}

TEST_F(RedoTest, ABtreeSplitReplaysEveryPageItNamesAndAnalysisDirtiesEach) {
    // Two leaves as a split leaves them: the old one linked to the new one.
    constexpr PageId kNew = kPage + 1;
    std::vector<std::byte> left(kPageSize);
    std::vector<std::byte> right(kPageSize);
    {
        auto l = heap::PageView::CreateEmptyAs(std::span<std::byte, kPageSize>(left.data(), kPageSize),
                                               1, PageType::kBtreeLeaf);
        ASSERT_TRUE(l.ok());
        ASSERT_TRUE(l.value().InsertTuple(Row(10), 7).ok());
        l.value().set_next_page_id(kNew);
        auto r = heap::PageView::CreateEmptyAs(
            std::span<std::byte, kPageSize>(right.data(), kPageSize), 20, PageType::kBtreeLeaf);
        ASSERT_TRUE(r.ok());
        ASSERT_TRUE(r.value().InsertTuple(Row(20), 7).ok());
    }
    const BtreeSplitImage images[] = {{kNew, right}, {kPage, left}};
    std::vector<std::byte> payload(BtreeSplitSize(2));
    ASSERT_TRUE(EncodeBtreeSplit(payload, images).ok());

    auto s = WalStream::Open(device_.get(), 0);
    ASSERT_TRUE(s.ok()) << s.status().message();
    ASSERT_TRUE(s.value()->Append({RecordType::kBtreeSplit, 7, kPage}, payload).ok());
    ASSERT_TRUE(s.value()->Sync().ok());

    const AnalysisResult analysis = Analyzed();
    EXPECT_EQ(analysis.dirty_pages.count(kPage), 1u);
    EXPECT_EQ(analysis.dirty_pages.count(kNew), 1u) << "the split's second page was not dirtied";
    EXPECT_EQ(analysis.max_page_id, kNew);

    auto redone = Redo(*device_, 0, store_, analysis);
    ASSERT_TRUE(redone.ok()) << redone.status().message();
    EXPECT_EQ(redone.value().page_images, 2u);
    EXPECT_EQ(LeafKeys(store_, kPage), std::vector<std::uint64_t>{10});
    EXPECT_EQ(LeafKeys(store_, kNew), std::vector<std::uint64_t>{20});
    auto old_leaf = store_.Get(kPage);
    ASSERT_TRUE(old_leaf.ok());
    EXPECT_EQ(heap::PageView(old_leaf.value().bytes()).next_page_id(), kNew)
        << "the link the split wrote did not replay with it";
}

// ---- The heal (wal.md §10) ------------------------------------------------
//
// On a `DevicePageStore`, because what refused the heal was the store's
// claim: a torn page is an allocated id whose device bytes are neither
// zeros nor a page that verifies.

class RedoHealTest : public RedoTest {
protected:
    std::unique_ptr<storage::MemoryPageDevice> pages_ =
        std::move(storage::MemoryPageDevice::Create(8, 0).value());

    std::unique_ptr<storage::DevicePageStore> OpenPages() {
        auto opened = storage::DevicePageStore::Open(*pages_, storage::FrameCapacity{4096},
                                                     server::kFirstUserPageId);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        return opened.ok() ? std::move(opened.value()) : nullptr;
    }

    // A heap page with `rows` tuples written to the device, then one body
    // byte flipped behind the store's back: the shape a torn write leaves.
    void WriteTornPage() {
        {
            auto store = OpenPages();
            ASSERT_NE(store, nullptr);
            {
                auto created = store->CreateAt(kPage);
                ASSERT_TRUE(created.ok()) << created.status().message();
                auto view = heap::PageView::CreateEmpty(created.value().bytes(), /*min_key=*/1);
                ASSERT_TRUE(view.ok());
                ASSERT_TRUE(view.value().InsertTuple(Bytes(24, 0x11), 3, 0).ok());
            }
            ASSERT_TRUE(store->Sync().ok());
        }
        std::array<std::byte, kPageSize> raw{};
        ASSERT_TRUE(pages_->ReadPage(kPage, std::span<std::byte, kPageSize>(raw)).ok());
        raw[kPageSize - 100] ^= std::byte{0xFF};
        ASSERT_TRUE(pages_->WritePage(kPage, std::span<const std::byte, kPageSize>(raw)).ok());
    }

    void AppendImage(WalStream& w, int rows) {
        std::vector<std::byte> image(kPageSize, std::byte{0});
        auto view = heap::PageView::CreateEmpty(
            std::span<std::byte, kPageSize>(image.data(), kPageSize), /*min_key=*/1);
        ASSERT_TRUE(view.ok());
        for (int i = 0; i < rows; ++i) {
            ASSERT_TRUE(view.value().InsertTuple(Bytes(24, 0x20 + i), 3, 0).ok());
        }
        std::vector<std::byte> buf(kFullPageImagePayloadSize, std::byte{0});
        ASSERT_TRUE(EncodeFullPageImage(
                        buf, std::span<const std::byte, kPageSize>(image.data(), kPageSize))
                        .ok());
        ASSERT_TRUE(w.Append({RecordType::kFullPageImage, 3, kPage}, buf).ok());
    }
};

TEST_F(RedoHealTest, AFullPageImageHealsAnAllocatedPageThatFailedItsChecksum) {
    WriteTornPage();
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();
        // A delta before the image poisons the page; the image heals it; the
        // delta after it lands on the healed bytes.
        ASSERT_NE(AppendHeapInsert(*s.value(), /*slot=*/1, 0x30), 0u);
        AppendImage(*s.value(), /*rows=*/1);
        ASSERT_NE(AppendHeapInsert(*s.value(), /*slot=*/1, 0x40), 0u);
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    auto store = OpenPages();
    ASSERT_NE(store, nullptr);
    ASSERT_EQ(store->Get(kPage).status().code(), StatusCode::kCorruption) << "the page is not torn";
    auto redone = Redo(*device_, 0, *store, Analyzed());
    ASSERT_TRUE(redone.ok()) << redone.status().message();
    EXPECT_EQ(redone.value().pages_healed, 1u);
    EXPECT_EQ(redone.value().page_images, 1u);

    auto page = store->Get(kPage);
    ASSERT_TRUE(page.ok()) << page.status().message();
    heap::PageView view(page.value().bytes());
    ASSERT_EQ(view.slot_count(), 2u) << "the delta after the image did not land";
    auto first = view.ReadTuple(0);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(std::vector<std::byte>(first.value().payload.begin(), first.value().payload.end()),
              Bytes(24, 0x20))
        << "slot 0 is not the image's";
    auto second = view.ReadTuple(1);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(std::vector<std::byte>(second.value().payload.begin(), second.value().payload.end()),
              Bytes(24, 0x40));
}

TEST_F(RedoHealTest, ATornPageNoImageRestoresIsStillRefusedByName) {
    WriteTornPage();
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();
        ASSERT_NE(AppendHeapInsert(*s.value(), /*slot=*/1, 0x30), 0u);
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto store = OpenPages();
    ASSERT_NE(store, nullptr);
    auto redone = Redo(*device_, 0, *store, Analyzed());
    ASSERT_FALSE(redone.ok());
    EXPECT_EQ(redone.status().code(), StatusCode::kCorruption) << redone.status().message();
    EXPECT_NE(redone.status().message().find("no full page image restored it"), std::string::npos)
        << redone.status().message();
}

TEST_F(RedoHealTest, OnlyTheHealTakesATornIdAndNeitherTakesALivePage) {
    WriteTornPage();
    {
        auto store = OpenPages();
        ASSERT_NE(store, nullptr);
        EXPECT_EQ(store->CreateAt(kPage).status().code(), StatusCode::kAlreadyExists)
            << "an ordinary create took a torn page";
        {
            auto healed = store->CreateOverTorn(kPage);
            ASSERT_TRUE(healed.ok()) << healed.status().message();
            storage::FormatPage(healed.value().bytes(), PageType::kHeap);
        }
        ASSERT_TRUE(store->Sync().ok());
    }
    // The id now holds a page that verifies, on the device and not resident.
    auto store = OpenPages();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->CreateOverTorn(kPage).status().code(), StatusCode::kAlreadyExists)
        << "the heal clobbered a page that verifies";
    EXPECT_TRUE(store->Get(kPage).ok());
}

}  // namespace
}  // namespace kds::wal
