#include "kds/wal/log_scanner.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/stream.hpp"

// RC01 - reading a stream back across segments
// (docs/workplan-wal-recovery.md).
//
// The three properties this task exists to establish, and they are the
// three a recovery phase would otherwise have to assume:
//
//   1. what was appended is what comes back, in order, with nothing lost
//      at a segment boundary and nothing delivered twice;
//   2. a torn tail stops the scan cleanly and says so, at *every* byte
//      offset of the last record rather than at one convenient one;
//   3. a segment header that does not belong to this stream fails the
//      scan, because that is not a crash - it is a different log.
//
// The scans below are driven against streams built by WalStream itself
// rather than by hand-written bytes. That is deliberate: a reader tested
// against a fixture the reader's own author encoded proves the author's
// idea of the format, not the format.

namespace kds::wal {
namespace {

// Small enough that a handful of records rolls a segment, and still a
// multiple of the record alignment (WalStream::Open requires it).
constexpr std::uint64_t kSegmentSize = 16 * 1024;

std::vector<std::byte> Payload(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

// Collects every record a scan yields, so a test can compare against what
// it appended.
struct Collected {
    std::vector<RecordType> types;
    std::vector<Lsn> lsns;
    std::vector<std::uint64_t> txn_ids;
    std::vector<std::vector<std::byte>> payloads;

    RecordVisitor Visitor() {
        return [this](const DecodedRecord& r) {
            types.push_back(r.type());
            lsns.push_back(r.header.lsn);
            txn_ids.push_back(r.header.txn_id);
            // Copied on purpose: the payload views the scanner's segment
            // buffer and does not outlive the call (log_scanner.hpp).
            payloads.emplace_back(r.payload.begin(), r.payload.end());
            return Status::OK();
        };
    }
};

class LogScannerTest : public ::testing::Test {
protected:
    // Construction is fallible and behind a factory, so the fixture holds
    // the owner and hands the tests the reference they already used.
    StatusOr<std::unique_ptr<MemoryLogDevice>> owned_device_ =
        MemoryLogDevice::Create(kSegmentSize);
    MemoryLogDevice& device_ = *owned_device_.value();
};

// ---- 1. Round trip, including across a roll ------------------------------

TEST_F(LogScannerTest, ReadsBackExactlyWhatWasAppended) {
    std::vector<Lsn> appended;
    {
        auto stream = WalStream::Open(&device_, /*core_id=*/0);
        ASSERT_TRUE(stream.ok()) << stream.status().message();
        for (int i = 0; i < 8; ++i) {
            auto lsn = stream.value()->Append(
                {RecordType::kHeapInsert, static_cast<std::uint64_t>(i + 1), 100u + i},
                Payload(64, static_cast<unsigned char>(i)));
            ASSERT_TRUE(lsn.ok()) << lsn.status().message();
            appended.push_back(lsn.value());
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    Collected got;
    auto outcome = ScanLog(device_, /*core_id=*/0, /*from_lsn=*/0, got.Visitor());
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();

    EXPECT_EQ(outcome.value().records, appended.size());
    ASSERT_EQ(got.lsns.size(), appended.size());
    EXPECT_EQ(got.lsns, appended);
    for (std::size_t i = 0; i < got.types.size(); ++i) {
        EXPECT_EQ(got.types[i], RecordType::kHeapInsert);
        EXPECT_EQ(got.txn_ids[i], i + 1);
        EXPECT_EQ(got.payloads[i], Payload(64, static_cast<unsigned char>(i)));
    }
    EXPECT_FALSE(outcome.value().stopped_early);
}

TEST_F(LogScannerTest, CrossesASegmentBoundaryLosingAndDuplicatingNothing) {
    // Payloads sized so the segment rolls several times: the boundary is
    // the case a one-buffer reader cannot see at all.
    constexpr std::size_t kBig = 3000;
    std::vector<Lsn> appended;
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok()) << stream.status().message();
        for (int i = 0; i < 20; ++i) {
            auto lsn = stream.value()->Append({RecordType::kHeapInsert, 7, 1},
                                              Payload(kBig, static_cast<unsigned char>(i)));
            ASSERT_TRUE(lsn.ok()) << lsn.status().message();
            appended.push_back(lsn.value());
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
    }
    ASSERT_GT(device_.segment_count(), 1u) << "the fixture did not roll; the test proves nothing";

    Collected got;
    auto outcome = ScanLog(device_, 0, 0, got.Visitor());
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();

    EXPECT_EQ(got.lsns, appended);
    EXPECT_EQ(outcome.value().records, appended.size());
    for (std::size_t i = 0; i < got.payloads.size(); ++i) {
        EXPECT_EQ(got.payloads[i], Payload(kBig, static_cast<unsigned char>(i)))
            << "record " << i << " came back with another record's bytes";
    }
}

TEST_F(LogScannerTest, APadMarkerIsFramingAndNeverReachesTheVisitor) {
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Seal().ok());  // writes the PAD
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 2, 2}).ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    Collected got;
    auto outcome = ScanLog(device_, 0, 0, got.Visitor());
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();

    ASSERT_EQ(got.types.size(), 2u);
    EXPECT_EQ(got.types[0], RecordType::kHeapInsert);
    EXPECT_EQ(got.types[1], RecordType::kHeapInsert);
    EXPECT_EQ(got.txn_ids[1], 2u) << "the record after the seal was not reached";
}

// ---- 2. The torn tail, at every byte offset ------------------------------

TEST_F(LogScannerTest, ATornTailStopsCleanlyAtEveryByteOffsetOfTheLastRecord) {
    // Build a stream, then truncate its last record one byte at a time.
    // A single truncation point would pass against a reader that happens
    // to fail on the byte it chose.
    std::vector<std::byte> good_segment;
    Lsn last_lsn = 0;
    std::uint64_t last_len = 0;
    {
        auto owned = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(owned.ok());
        MemoryLogDevice& device = *owned.value();
        auto stream = WalStream::Open(&device, 0);
        ASSERT_TRUE(stream.ok());
        for (int i = 0; i < 4; ++i) {
            auto lsn = stream.value()->Append({RecordType::kHeapInsert, 1, 1}, Payload(32, 0xAB));
            ASSERT_TRUE(lsn.ok());
            last_lsn = lsn.value();
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
        last_len = stream.value()->durable_lsn() - last_lsn;

        good_segment.resize(static_cast<std::size_t>(kSegmentSize));
        ASSERT_TRUE(device.ReadAt(0, 0, good_segment).ok());
    }
    ASSERT_GT(last_len, 0u);

    for (std::uint64_t cut = 1; cut <= last_len; ++cut) {
        auto owned = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(owned.ok());
        MemoryLogDevice& device = *owned.value();
        ASSERT_TRUE(device.CreateSegment(0).ok());
        // Everything up to the last record, plus `last_len - cut` of it.
        std::vector<std::byte> truncated = good_segment;
        const std::size_t keep = static_cast<std::size_t>(last_lsn + (last_len - cut));
        for (std::size_t i = keep; i < truncated.size(); ++i) {
            truncated[i] = std::byte{0};
        }
        ASSERT_TRUE(device.WriteAt(0, 0, truncated).ok());

        Collected got;
        auto outcome = ScanLog(device, 0, 0, got.Visitor());
        ASSERT_TRUE(outcome.ok()) << "cut " << cut << ": " << outcome.status().message();
        // The three records before it always survive; the torn one never
        // reaches the visitor.
        EXPECT_EQ(outcome.value().records, 3u) << "cut " << cut;
        EXPECT_EQ(outcome.value().end_lsn, last_lsn) << "cut " << cut;
    }
}

TEST_F(LogScannerTest, AZeroedTailIsACleanEndNotATornOne) {
    // Bytes never written read as zeroes, which is what a fresh segment's
    // slack is. That must read as "the stream ends here", not as damage.
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }
    auto outcome = ScanLogToEnd(device_, 0, 0);
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();
    EXPECT_EQ(outcome.value().records, 1u);
    EXPECT_FALSE(outcome.value().stopped_early);
}

// ---- 3. A foreign segment fails the scan, and is not a torn tail --------

TEST_F(LogScannerTest, ASegmentBelongingToAnotherCoreIsCorruptionNotAnEnd) {
    {
        auto stream = WalStream::Open(&device_, /*core_id=*/3);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    // Scanned as core 0's stream: same bytes, wrong owner.
    auto outcome = ScanLogToEnd(device_, /*core_id=*/0, 0);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status().code(), StatusCode::kCorruption) << outcome.status().message();
    EXPECT_NE(outcome.status().message().find("belongs to core 3"), std::string::npos)
        << outcome.status().message();
}

// ---- Start position ------------------------------------------------------

TEST_F(LogScannerTest, AScanStartsAtTheGivenLsnAndNotBefore) {
    std::vector<Lsn> appended;
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        for (int i = 0; i < 6; ++i) {
            auto lsn = stream.value()->Append(
                {RecordType::kHeapInsert, static_cast<std::uint64_t>(i + 1), 1}, Payload(48, 0));
            ASSERT_TRUE(lsn.ok());
            appended.push_back(lsn.value());
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    Collected got;
    auto outcome = ScanLog(device_, 0, appended[3], got.Visitor());
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();
    EXPECT_EQ(outcome.value().records, 3u);
    ASSERT_EQ(got.lsns.size(), 3u);
    EXPECT_EQ(got.lsns[0], appended[3]);
    EXPECT_EQ(got.txn_ids[0], 4u);
}

TEST_F(LogScannerTest, AnLsnInsideTheHeaderBlockIsRefusedRatherThanRounded) {
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }
    auto outcome = ScanLogToEnd(device_, 0, /*from_lsn=*/64);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(LogScannerTest, AnLsnPastEveryCreatedSegmentIsRefused) {
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }
    auto outcome = ScanLogToEnd(device_, 0, /*from_lsn=*/kSegmentSize * 9 + kSegmentHeaderSize);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(LogScannerTest, AnEmptyDeviceScansToTheFirstLegalRecordPosition) {
    auto outcome = ScanLogToEnd(device_, 0, 0);
    ASSERT_TRUE(outcome.ok()) << outcome.status().message();
    EXPECT_EQ(outcome.value().records, 0u);
    EXPECT_EQ(outcome.value().end_lsn, kSegmentHeaderSize);
}

// ---- The visitor's veto --------------------------------------------------

TEST_F(LogScannerTest, AVisitorsErrorStopsTheScanAndIsReturned) {
    // wal.md section 5.2: an unknown record type during replay is a hard
    // recovery error, never skipped. The scanner does not decide that - it
    // gives the phase above it a way to say so.
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    int seen = 0;
    auto outcome = ScanLog(device_, 0, 0, [&seen](const DecodedRecord&) {
        if (++seen == 2) return Status::Corruption("visitor said stop");
        return Status::OK();
    });
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status().code(), StatusCode::kCorruption);
    EXPECT_EQ(seen, 2) << "the scan kept going after the visitor refused";
}

// ---- The reuse control ---------------------------------------------------

TEST_F(LogScannerTest, TheScannerAndTheAppendPathAgreeAboutTheDurableEnd) {
    // ValidateSegmentHeader is shared with WalStream::ScanTail, and this is
    // the property that sharing exists to keep: a stream reopened for
    // append resumes exactly where a scan says the records end. Without it
    // recovery could replay a record the writer is about to overwrite.
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}, Payload(80, 1))
                            .ok());
        }
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    auto scanned = ScanLogToEnd(device_, 0, 0);
    ASSERT_TRUE(scanned.ok()) << scanned.status().message();

    auto reopened = WalStream::Open(&device_, 0);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(scanned.value().end_lsn, reopened.value()->append_lsn());
}

TEST_F(LogScannerTest, TheyAlsoAgreeAfterASealedTailSegment) {
    // The sealed case has its own arithmetic on both sides (the stream
    // jumps to the next segment boundary), so it gets its own assertion.
    {
        auto stream = WalStream::Open(&device_, 0);
        ASSERT_TRUE(stream.ok());
        ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 1}).ok());
        ASSERT_TRUE(stream.value()->Seal().ok());
        ASSERT_TRUE(stream.value()->Sync().ok());
    }

    auto scanned = ScanLogToEnd(device_, 0, 0);
    ASSERT_TRUE(scanned.ok()) << scanned.status().message();
    EXPECT_TRUE(scanned.value().sealed);

    auto reopened = WalStream::Open(&device_, 0);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(scanned.value().end_lsn, reopened.value()->append_lsn());
}

}  // namespace
}  // namespace kds::wal
