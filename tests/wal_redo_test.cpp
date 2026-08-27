#include "kds/wal/redo.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/anchor_page.hpp"
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

TEST_F(RedoTest, AHandedOffPagesRecordsAreSkippedAndThePageNeverFaulted) {
    // PW1c-2, page-lsn-cross-stream.md section 9 rule 3's redo half.
    // The stream holds PAGE_INIT + two inserts for kPage and then a
    // PAGE_HANDOFF: the page left this stream, so redo must apply nothing
    // for it and must not even create or load it. The store deliberately
    // does not hold the page, which makes any touch loud - an unfiltered
    // redo would CreateAt the page from its PAGE_INIT and apply all three
    // records.
    WriteHeapStream(2);
    {
        // An ordinary stream, plus a handoff: Open resumes at the durable
        // tail, so this appends after the helper's records.
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(
            s.value()->Append({RecordType::kPageHandoff, kNoTxnId, kPage}, handoff).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    AnalysisResult analysis = Analyzed();
    ASSERT_EQ(analysis.dirty_pages.count(kPage), 0u)
        << "the handoff must have removed the page from the dirty page table";
    // The table is empty, so RedoStartFrom answered end_lsn and redo would
    // correctly visit nothing. What is under test is the per-record filter
    // - that a visited record for a departed page is skipped unfaulted -
    // so scan from the head, the TransactionAndCheckpointRecords test's
    // arrangement.
    analysis.redo_start_lsn = 0;

    auto r = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 0u);
    EXPECT_EQ(r.value().skipped_not_dirty, 3u) << "init + two inserts";
    EXPECT_FALSE(store_.Get(kPage).ok())
        << "redo must not create or fault a page that left the stream";
}

// ---- The PL-C stamp (PW1c-3, page-lsn-cross-stream.md §9 4-5) -------

TEST_F(RedoTest, AnAppliedRecordStampsTheOwningStream) {
    // Rule 4: the stream stamp rides the page_lsn stamp, so a replayed
    // page names the stream whose byte offsets its page_lsn is in.
    WriteHeapStream(1);
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_GT(r.value().applied, 0u);
    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::GetPageStreamStamp(page.value().bytes()), 1u);  // core 0 + 1
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

TEST_F(RedoTest, AForeignStampReachableByRedoRefusesTheMount) {
    // Rule 5, at full strength since rule 6 (the acquisition restamp): a
    // foreign stamp inside this stream's redo scope has no benign reading
    // - every legitimate crossing restamps durably before this stream's
    // first record for the page exists - so redo refuses rather than
    // compare incomparable page_lsns, the spec's §3 silent corruption
    // made loud. The second pass revisits records RV5 would have skipped;
    // the stamp check must fire first, because "a mismatch redo can
    // reach" includes exactly those.
    WriteHeapStream(1);
    ASSERT_TRUE(Redo((*device_), 0, store_, Analyzed()).ok());
    {
        auto page = store_.Get(kPage);
        ASSERT_TRUE(page.ok());
        storage::SetPageStreamStamp(page.value().bytes(), 3);  // stream 2's
    }
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

TEST_F(RedoTest, AForeignStampRefusesEvenWithAHandoffInTheWindow) {
    // The shape rule 5a (retracted same-day at the f19ead1 review's C2)
    // would have admitted: foreign stamp, a PAGE_HANDOFF in the scanned
    // range, post-return records above it. Under rule 6 the legitimate
    // return restamps at re-acquisition, so this durable state can only
    // mean the restamp was lost - refused, never applied over another
    // stream's data.
    WriteHeapStream(1);
    ASSERT_TRUE(Redo((*device_), 0, store_, Analyzed()).ok());
    {
        auto page = store_.Get(kPage);
        ASSERT_TRUE(page.ok());
        storage::SetPageStreamStamp(page.value().bytes(), 2);  // stream 1's
        storage::SetPageLsn(page.value().bytes(), 0x7FFFFFFFFFFFULL);
    }
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(
            s.value()->Append({RecordType::kPageHandoff, kNoTxnId, kPage}, handoff).ok());
        ASSERT_NE(AppendHeapInsert(*s.value(), /*slot=*/1, 0xC5), 0u);
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Redo((*device_), 0, store_, Analyzed());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

TEST_F(RedoTest, ARestampedReturnedPageReplaysItsPostReturnRecordsNormally) {
    // The legitimate A→B→A, as rule 6 leaves it at mount: the
    // re-acquisition restamped the page to this stream and re-based
    // page_lsn into this stream's space (here: below the post-return
    // record, as a restamp taken at the stream's then-current end always
    // is), so the post-return record passes the ordinary RV5 gate - no
    // bypass, no special case.
    WriteHeapStream(1);
    ASSERT_TRUE(Redo((*device_), 0, store_, Analyzed()).ok());
    Lsn restamp_base = 0;
    {
        auto page = store_.Get(kPage);
        ASSERT_TRUE(page.ok());
        restamp_base = storage::GetPageLsn(page.value().bytes());
        storage::SetPageStreamStamp(page.value().bytes(), 1);  // own, per rule 6
    }
    Lsn returned = 0;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(
            s.value()->Append({RecordType::kPageHandoff, kNoTxnId, kPage}, handoff).ok());
        returned = AppendHeapInsert(*s.value(), /*slot=*/1, 0xC5);
        ASSERT_NE(returned, 0u);
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    ASSERT_GT(returned, restamp_base);

    const AnalysisResult analysis = Analyzed();
    ASSERT_EQ(analysis.dirty_pages.at(kPage), returned);

    auto r = Redo((*device_), 0, store_, analysis);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().applied, 1u) << "the post-return record replays";

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::GetPageStreamStamp(page.value().bytes()), 1u);
    EXPECT_EQ(storage::GetPageLsn(page.value().bytes()), returned);
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

}  // namespace
}  // namespace kds::wal
