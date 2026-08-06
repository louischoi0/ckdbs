#include "kds/txn/undo_log.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// docs/txn.md section 10-1 (the log half) and 10-4: appends chain pages
// when one fills, a walk is bounded rather than hanging, the UNDO_WRITE
// mapping round-trips, and an undo page image rebuilt from the records read
// **off the device** is identical to the live page.

namespace kds::txn {
namespace {

constexpr std::uint64_t kSegmentSize = 1 << 20;
constexpr PageId kFirstUserPageId = 128;

std::vector<std::byte> BytesOf(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string StringOf(std::span<const std::byte> b) {
    std::string out(b.size(), '\0');
    std::memcpy(out.data(), b.data(), b.size());
    return out;
}

UndoRecordFields Overwrite(std::uint64_t prior_trx_id, std::uint64_t prior_undo_ptr,
                           PageId target_page_id, std::uint16_t target_slot) {
    UndoRecordFields r{};
    r.prior_trx_id = prior_trx_id;
    r.prior_undo_ptr = prior_undo_ptr;
    r.target_page_id = target_page_id;
    r.target_slot = target_slot;
    r.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
    return r;
}

class UndoLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto page_device = storage::MemoryPageDevice::Create(/*extent_pages=*/64,
                                                             /*initial_pages=*/64);
        ASSERT_TRUE(page_device.ok()) << page_device.status().message();
        device_ = std::move(page_device.value());

        auto log_device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log_device.ok()) << log_device.status().message();
        log_device_ = std::move(log_device.value());

        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetWalGate(wal_.get());

        log_ = std::make_unique<UndoLog>(*store_, wal_.get());
    }

    // Every record the *device* holds, in stream order - read back through
    // the device rather than asked of the manager, for the reason
    // insert_wal_test.cpp gives: what the manager believes it appended is
    // not evidence of what a crash leaves.
    std::vector<wal::DecodedRecord> DeviceRecords(std::vector<std::vector<std::byte>>& storage) {
        // Nothing here commits, so nothing has forced the ring out. The
        // undo log is not a transaction and does not own the commit - the
        // caller that stamped the tuple does - so the sync is the test's
        // job here, exactly as a real caller's TXN_COMMIT would be.
        EXPECT_TRUE(wal_->SyncAll().ok());
        std::vector<wal::DecodedRecord> found;
        for (std::uint64_t seg = 0; seg < log_device_->segment_count(); ++seg) {
            storage.emplace_back(kSegmentSize - wal::kSegmentHeaderSize);
            std::vector<std::byte>& body = storage.back();
            EXPECT_TRUE(log_device_->ReadAt(seg, wal::kSegmentHeaderSize, body).ok());
            wal::RecordReader reader(body, seg * kSegmentSize + wal::kSegmentHeaderSize);
            while (std::optional<wal::DecodedRecord> record = reader.Next()) {
                if (record->type() == wal::RecordType::kPad) break;
                found.push_back(*record);
            }
        }
        return found;
    }

    sched::ManualClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::unique_ptr<UndoLog> log_;
};

TEST_F(UndoLogTest, AnAppendedImageReadsBackThroughItsUndoPtr) {
    auto ptr = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("before"));
    ASSERT_TRUE(ptr.ok()) << ptr.status().message();
    ASSERT_TRUE(UndoPtrIsPlausible(ptr.value()).ok());

    auto version = log_->Read(ptr.value());
    ASSERT_TRUE(version.ok()) << version.status().message();
    EXPECT_EQ(version.value().type, UndoRecordType::kOverwrite);
    EXPECT_EQ(version.value().prior_trx_id, 41u);
    EXPECT_EQ(version.value().prior_undo_ptr, kNoUndoPtr);
    EXPECT_EQ(StringOf(version.value().image), "before");
}

// The page is allocated with CreateNew + FormatUndoPage, never
// CreateNewHeaderless - so it carries a validatable kUndo header and a
// page_lsn the WAL gate can read (txn.md section 3.1).
TEST_F(UndoLogTest, UndoPagesAreHeaderedAndStamped) {
    auto ptr = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("before"));
    ASSERT_TRUE(ptr.ok());

    auto page = store_->Get(UndoPtrPageId(ptr.value()));
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(storage::ValidatePageHeader(std::span<const std::byte, kPageSize>(page.value()),
                                            PageType::kUndo)
                    .ok());
    EXPECT_NE(storage::GetPageLsn(page.value()), storage::kNoPageLsn);

    const UndoPageHeaderFields h =
        ReadUndoPageHeader(std::span<const std::byte, kPageSize>(page.value()));
    // The creating transaction, recorded as a diagnostic - it owns nothing.
    EXPECT_EQ(h.first_trx_id, 50u);
    EXPECT_EQ(h.prev_page_id, kInvalidPageId);
}

TEST_F(UndoLogTest, AppendsShareAPageUntilItFills) {
    // 2000-byte images: 2028 bytes per record, four to a page.
    const std::vector<std::byte> image(2000, std::byte{0x5A});
    std::vector<PageId> pages;
    for (int i = 0; i < 5; ++i) {
        auto ptr = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), image);
        ASSERT_TRUE(ptr.ok()) << ptr.status().message();
        pages.push_back(UndoPtrPageId(ptr.value()));
    }
    EXPECT_EQ(pages[0], pages[1]);
    EXPECT_EQ(pages[0], pages[2]);
    EXPECT_EQ(pages[0], pages[3]);
    EXPECT_NE(pages[0], pages[4]) << "the fifth record must land on a fresh page";

    auto count = log_->PageCount();
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.value(), 2u);

    // The page chain runs newest -> oldest through prev_page_id, and it is
    // a *reclamation* chain: nothing reads a version through it.
    auto tail = store_->Get(pages[4]);
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(ReadUndoPageHeader(std::span<const std::byte, kPageSize>(tail.value())).prev_page_id,
              pages[0]);
}

// Transactions **share** a page, which is the whole point: an autocommitted
// statement is a transaction, so a page per transaction is 8 KB per UPDATE
// (undo_log.hpp). Three transactions, three small records, one page.
TEST_F(UndoLogTest, TransactionsShareTheCurrentPage) {
    auto a = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("a"));
    auto b = log_->Append(51, Overwrite(41, kNoUndoPtr, 300, 3), BytesOf("b"));
    auto c = log_->Append(52, Overwrite(41, kNoUndoPtr, 300, 4), BytesOf("c"));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(UndoPtrPageId(a.value()), UndoPtrPageId(b.value()));
    EXPECT_EQ(UndoPtrPageId(a.value()), UndoPtrPageId(c.value()));
    EXPECT_EQ(log_->PageCount().value(), 1u);

    // Sharing a page changes nothing about reading one: each record is
    // reached by its own undo_ptr, and no reader consults the page's header.
    EXPECT_EQ(StringOf(log_->Read(a.value()).value().image), "a");
    EXPECT_EQ(StringOf(log_->Read(b.value()).value().image), "b");
    EXPECT_EQ(StringOf(log_->Read(c.value()).value().image), "c");
}

// The saving, stated as the test that would have caught the old behaviour:
// 200 autocommit-shaped transactions of one small record each must not cost
// 200 pages.
TEST_F(UndoLogTest, ManyShortTransactionsDoNotCostAPageEach) {
    for (std::uint64_t trx_id = 100; trx_id < 300; ++trx_id) {
        ASSERT_TRUE(log_->Append(trx_id, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("image"))
                        .ok());
    }
    EXPECT_EQ(log_->PageCount().value(), 1u)
        << "200 records of 33 bytes fit on one 8,136-byte page";
}

// A version chain crosses transactions freely - which is the difference
// between it and the page chain above.
TEST_F(UndoLogTest, AVersionChainWalksNewestToOldestAcrossTransactions) {
    auto v1 = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("v1"));
    ASSERT_TRUE(v1.ok());
    auto v2 = log_->Append(51, Overwrite(50, v1.value(), 300, 2), BytesOf("v2"));
    ASSERT_TRUE(v2.ok());
    auto v3 = log_->Append(52, Overwrite(51, v2.value(), 300, 2), BytesOf("v3"));
    ASSERT_TRUE(v3.ok());

    std::vector<std::string> seen;
    ASSERT_TRUE(log_->Walk(v3.value(), [&](const UndoVersion& v) {
                        seen.push_back(StringOf(v.image));
                        return true;
                    })
                    .ok());
    EXPECT_EQ(seen, (std::vector<std::string>{"v3", "v2", "v1"}));
}

TEST_F(UndoLogTest, AWalkStopsWhenTheVisitorSaysSo) {
    auto v1 = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("v1"));
    ASSERT_TRUE(v1.ok());
    auto v2 = log_->Append(50, Overwrite(50, v1.value(), 300, 2), BytesOf("v2"));
    ASSERT_TRUE(v2.ok());

    int visits = 0;
    ASSERT_TRUE(log_->Walk(v2.value(), [&](const UndoVersion&) {
                        ++visits;
                        return false;
                    })
                    .ok());
    EXPECT_EQ(visits, 1);
}

// A self-referential link is Corruption, not a hang - the same guard the
// heap chain has (txn.md section 3.7).
TEST_F(UndoLogTest, ASelfReferentialLinkIsCorruptionNotAHang) {
    auto ptr = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("v1"));
    ASSERT_TRUE(ptr.ok());

    // Point the record's prior_undo_ptr at itself, in place.
    auto page = store_->Get(UndoPtrPageId(ptr.value()));
    ASSERT_TRUE(page.ok());
    const std::uint64_t self = ptr.value();
    std::memcpy(page.value().data() + UndoPtrOffset(ptr.value()) + kUndoRecPriorUndoPtrOffset,
                &self, sizeof(self));

    Status walked = log_->Walk(ptr.value(), [](const UndoVersion&) { return true; });
    EXPECT_EQ(walked.code(), StatusCode::kCorruption) << walked.message();
}

TEST_F(UndoLogTest, ReadRefusesAnImplausiblePointer) {
    EXPECT_EQ(log_->Read(kNoUndoPtr).status().code(), StatusCode::kCorruption);
    EXPECT_EQ(log_->Read(EncodeUndoPtr(0, 1024)).status().code(), StatusCode::kCorruption);
}

TEST_F(UndoLogTest, AnOversizeImageIsRefusedWithoutLeakingAPage) {
    const std::vector<std::byte> huge(kMaxUndoImageLen + 1, std::byte{0x01});
    auto refused = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), huge);
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(log_->PageCount().value(), 0u);
}

// ---- The WAL mapping (txn.md section 3.5) --------------------------------

TEST_F(UndoLogTest, AnAppendLogsPageInitThenUndoWrite) {
    std::vector<std::vector<std::byte>> storage;
    const std::size_t before = DeviceRecords(storage).size();
    storage.clear();

    auto ptr = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("before"));
    ASSERT_TRUE(ptr.ok());

    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);
    ASSERT_EQ(records.size(), before + 2)
        << "a first append is PAGE_INIT for the page plus UNDO_WRITE for the record";
    EXPECT_EQ(records[before + 0].type(), wal::RecordType::kPageInit);
    EXPECT_EQ(records[before + 1].type(), wal::RecordType::kUndoWrite);

    auto init = wal::DecodePageInit(records[before].payload);
    ASSERT_TRUE(init.ok());
    EXPECT_EQ(init.value().page_type, static_cast<std::uint8_t>(PageType::kUndo));
    EXPECT_EQ(init.value().min_key, 0u);

    // The envelope names the **undo** page, not the heap page the record
    // describes - and the payload's prior writer is a different
    // transaction from the envelope's, which is the one exception
    // payload.hpp already documents.
    EXPECT_EQ(records[before + 1].header.page_id, UndoPtrPageId(ptr.value()));
    EXPECT_EQ(records[before + 1].header.txn_id, 50u);

    auto write = wal::DecodeUndoWrite(records[before + 1].payload);
    ASSERT_TRUE(write.ok());
    EXPECT_EQ(write.value().fields.prior_trx_id, 41u);
    EXPECT_EQ(write.value().fields.prior_undo_ptr, kNoUndoPtr);
    EXPECT_EQ(write.value().fields.offset, UndoPtrOffset(ptr.value()));
    EXPECT_EQ(StringOf(write.value().image), "before");
}

TEST_F(UndoLogTest, NoFullPageImageIsEverEmittedForAnUndoPage) {
    const std::vector<std::byte> image(2000, std::byte{0x5A});
    for (int i = 0; i < 6; ++i) {  // enough to force a second page
        ASSERT_TRUE(log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), image).ok());
    }
    std::vector<std::vector<std::byte>> storage;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        EXPECT_NE(record.type(), wal::RecordType::kFullPageImage)
            << "an undo page is reconstructible from PAGE_INIT + UNDO_WRITEs, so it is "
               "FPI-exempt on the merits";
    }
}

// txn.md section 10-4: the records read **off the device** are decoded, an
// undo page image is rebuilt from those records alone, and it is asserted
// identical to the live page. This is what makes "lower and nr_records are
// derivable by replaying a page's UNDO_WRITEs in LSN order" a fact rather
// than a claim - no undo-page-header record type is needed because of it.
TEST_F(UndoLogTest, APageRebuiltFromItsRecordsAloneMatchesTheLivePage) {
    const std::vector<std::byte> image(500, std::byte{0x33});
    std::uint64_t last = kNoUndoPtr;
    for (int i = 0; i < 5; ++i) {
        auto ptr = log_->Append(50, Overwrite(41, last, 300, 2), image);
        ASSERT_TRUE(ptr.ok()) << ptr.status().message();
        last = ptr.value();
    }
    const PageId page_id = UndoPtrPageId(last);

    // Replay: PAGE_INIT formats, each UNDO_WRITE writes at its named
    // offset. Nothing else is consulted.
    std::array<std::byte, kPageSize> rebuilt{};
    std::vector<std::vector<std::byte>> storage;
    bool initialized = false;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        if (record.header.page_id != page_id) continue;
        if (record.type() == wal::RecordType::kPageInit) {
            auto init = wal::DecodePageInit(record.payload);
            ASSERT_TRUE(init.ok());
            ASSERT_EQ(init.value().page_type, static_cast<std::uint8_t>(PageType::kUndo));
            ASSERT_TRUE(FormatUndoPage(std::span<std::byte, kPageSize>(rebuilt),
                                       record.header.txn_id, kInvalidPageId)
                            .ok());
            initialized = true;
            continue;
        }
        if (record.type() != wal::RecordType::kUndoWrite) continue;
        ASSERT_TRUE(initialized) << "an UNDO_WRITE arrived before its page's PAGE_INIT";
        auto write = wal::DecodeUndoWrite(record.payload);
        ASSERT_TRUE(write.ok());

        // The record header's two chain-link fields come from the payload;
        // the target and the type are what the *image* would have to carry
        // if they were not already on the page, and they are - which is
        // why replay reads them back off the rebuilt page below rather
        // than inventing them here.
        UndoRecordFields fields{};
        fields.prior_trx_id = write.value().fields.prior_trx_id;
        fields.prior_undo_ptr = write.value().fields.prior_undo_ptr;
        fields.target_page_id = 300;
        fields.target_slot = 2;
        fields.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
        ASSERT_TRUE(UndoPageWriteAt(std::span<std::byte, kPageSize>(rebuilt),
                                    write.value().fields.offset, fields, write.value().image)
                        .ok());
    }
    ASSERT_TRUE(initialized);

    auto live = store_->Get(page_id);
    ASSERT_TRUE(live.ok());

    // The bodies must match byte for byte: lower and nr_records included,
    // which is the whole claim. The common page header is excluded because
    // page_lsn and the checksum are properties of the log, not of the
    // records - a rebuilt page has never been written to.
    EXPECT_EQ(std::memcmp(rebuilt.data() + storage::kPageBodyOffset,
                          live.value().data() + storage::kPageBodyOffset,
                          kPageSize - storage::kPageBodyOffset),
              0);

    const UndoPageHeaderFields rh =
        ReadUndoPageHeader(std::span<const std::byte, kPageSize>(rebuilt));
    const UndoPageHeaderFields lh =
        ReadUndoPageHeader(std::span<const std::byte, kPageSize>(live.value()));
    EXPECT_EQ(rh.lower, lh.lower);
    EXPECT_EQ(rh.nr_records, lh.nr_records);
}

// An UndoLog built without a WalManager mutates pages and logs nothing -
// the unlogged path every pre-existing socket-free test runs on.
TEST_F(UndoLogTest, AnUnloggedLogStillAppendsAndReads) {
    UndoLog unlogged(*store_, /*wal=*/nullptr);
    auto ptr = unlogged.Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("before"));
    ASSERT_TRUE(ptr.ok()) << ptr.status().message();

    auto version = unlogged.Read(ptr.value());
    ASSERT_TRUE(version.ok());
    EXPECT_EQ(StringOf(version.value().image), "before");
}

// A finished transaction releases nothing, and there is no longer an API
// that could pretend otherwise: its records stay readable through any
// tuple's undo_ptr, and the page it wrote to goes on serving the next
// transaction (txn.md section 6).
TEST_F(UndoLogTest, AFinishedTransactionsRecordsStayReadableAndItsPageStaysInUse) {
    auto old = log_->Append(50, Overwrite(41, kNoUndoPtr, 300, 2), BytesOf("before"));
    ASSERT_TRUE(old.ok());

    auto next = log_->Append(51, Overwrite(50, old.value(), 300, 2), BytesOf("after"));
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(UndoPtrPageId(next.value()), UndoPtrPageId(old.value()));

    auto version = log_->Read(old.value());
    ASSERT_TRUE(version.ok());
    EXPECT_EQ(StringOf(version.value().image), "before");
}

}  // namespace
}  // namespace kds::txn
