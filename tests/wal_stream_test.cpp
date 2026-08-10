#include "kds/wal/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The stream's two jobs: put a record exactly where the LSN it returned
// says it is, and never claim more is durable than the device has synced.

namespace kds::wal {
namespace {

// Big enough for several records per segment, small enough that filling one
// is a handful of appends.
constexpr std::uint64_t kSegmentSize = 65536;
constexpr std::size_t kPayloadSize = 1000;

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 13u) & 0xFF);
    }
    return bytes;
}

RecordSpec HeapInsert(std::uint64_t txn_id, PageId page_id) {
    return RecordSpec{RecordType::kHeapInsert, txn_id, page_id, 0};
}

class WalStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(created.ok()) << created.status().message();
        device_ = std::move(created.value());
    }

    std::unique_ptr<WalStream> OpenStream(std::uint32_t core_id = 0,
                                          std::size_t ring = kMinRingCapacity) {
        auto opened = WalStream::Open(device_.get(), core_id, ring);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        return opened.ok() ? std::move(opened.value()) : nullptr;
    }

    // Reads the record the stream says it placed at `lsn`, straight off the
    // device, and returns its payload. This is the assertion that matters:
    // the LSN is an address, not a receipt.
    std::vector<std::byte> ReadRecordAt(Lsn lsn, RecordType expected_type) {
        const std::uint64_t segment_no = lsn / kSegmentSize;
        const std::uint64_t offset = lsn % kSegmentSize;
        std::vector<std::byte> bytes(kSegmentSize - offset);
        EXPECT_TRUE(device_->ReadAt(segment_no, offset, bytes).ok());

        auto decoded = DecodeRecord(bytes);
        EXPECT_TRUE(decoded.ok()) << decoded.status().message();
        if (!decoded.ok()) {
            return {};
        }
        EXPECT_EQ(decoded.value().header.lsn, lsn);
        EXPECT_EQ(decoded.value().type(), expected_type);
        const auto payload = decoded.value().payload;
        return std::vector<std::byte>(payload.begin(), payload.end());
    }

    std::unique_ptr<MemoryLogDevice> device_;
};

// ---- Opening -------------------------------------------------------------

TEST_F(WalStreamTest, FreshOpenCreatesSegmentZeroWithAValidHeader) {
    auto stream = OpenStream(4);
    ASSERT_NE(stream, nullptr);

    EXPECT_EQ(device_->segment_count(), 1u);
    // The first record of segment 0 sits just past the header block, so no
    // record ever has LSN 0 - which is what keeps page_lsn 0 meaning
    // "never logged".
    EXPECT_EQ(stream->append_lsn(), kSegmentHeaderSize);
    EXPECT_EQ(stream->flushed_lsn(), kSegmentHeaderSize);
    EXPECT_EQ(stream->durable_lsn(), kSegmentHeaderSize);
    EXPECT_FALSE(stream->sealed());

    std::vector<std::byte> header(kSegmentHeaderSize);
    ASSERT_TRUE(device_->ReadAt(0, 0, header).ok());
    auto decoded = DecodeSegmentHeader(header);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().core_id, 4u);
    EXPECT_EQ(decoded.value().segment_no, 0u);
    EXPECT_EQ(decoded.value().start_lsn, 0u);
}

TEST_F(WalStreamTest, OpenRejectsATooSmallRing) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity - 1);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(WalStreamTest, OpenRejectsAnUnusableSegmentSize) {
    auto tiny = MemoryLogDevice::Create(kSegmentHeaderSize);
    ASSERT_TRUE(tiny.ok());
    auto opened = WalStream::Open(tiny.value().get());
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);

    // An unaligned segment size would put records at unaligned LSNs.
    auto odd = MemoryLogDevice::Create(kSegmentSize + 1);
    ASSERT_TRUE(odd.ok());
    auto opened_odd = WalStream::Open(odd.value().get());
    ASSERT_FALSE(opened_odd.ok());
    EXPECT_EQ(opened_odd.status().code(), StatusCode::kInvalidArgument);
}

// ---- Append and LSN arithmetic -------------------------------------------

TEST_F(WalStreamTest, AppendReturnsTheAddressTheRecordIsWrittenTo) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 1);

    auto lsn = stream->Append(HeapInsert(7, 42), payload);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_EQ(lsn.value(), kSegmentHeaderSize);
    ASSERT_TRUE(stream->Flush().ok());

    const auto read_back = ReadRecordAt(lsn.value(), RecordType::kHeapInsert);
    ASSERT_GE(read_back.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), read_back.begin()));
}

TEST_F(WalStreamTest, LsnsAdvanceByTheEncodedRecordSize) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 2);
    const std::size_t step = EncodedRecordSize(payload.size());

    Lsn expected = kSegmentHeaderSize;
    for (std::uint64_t txn = 1; txn <= 5; ++txn) {
        auto lsn = stream->Append(HeapInsert(txn, static_cast<PageId>(txn)), payload);
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
        EXPECT_EQ(lsn.value(), expected);
        // Every LSN is 8-byte aligned because every record is.
        EXPECT_EQ(lsn.value() % kRecordAlignment, 0u);
        expected += step;
    }
    EXPECT_EQ(stream->append_lsn(), expected);
}

TEST_F(WalStreamTest, StagedBytesAreNotOnTheDeviceUntilFlush) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 3);

    auto lsn = stream->Append(HeapInsert(1, 1), payload);
    ASSERT_TRUE(lsn.ok());
    EXPECT_EQ(stream->ring_used(), EncodedRecordSize(payload.size()));
    EXPECT_EQ(stream->flushed_lsn(), kSegmentHeaderSize);
    EXPECT_EQ(device_->stats().writes, 1u);  // only the segment header so far

    ASSERT_TRUE(stream->Flush().ok());
    EXPECT_EQ(stream->ring_used(), 0u);
    EXPECT_EQ(stream->flushed_lsn(), stream->append_lsn());
    EXPECT_EQ(device_->stats().writes, 2u);

    // Batched: many records, one write.
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(stream->Append(HeapInsert(2, 2), payload).ok());
    }
    ASSERT_TRUE(stream->Flush().ok());
    EXPECT_EQ(device_->stats().writes, 3u);
}

TEST_F(WalStreamTest, PayloadlessRecordsAppendToo) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);

    auto begin = stream->Append({RecordType::kTxnBegin, 11, kInvalidPageId, 0});
    ASSERT_TRUE(begin.ok()) << begin.status().message();
    auto commit = stream->Append({RecordType::kTxnCommit, 11, kInvalidPageId, 0});
    ASSERT_TRUE(commit.ok());
    EXPECT_EQ(commit.value() - begin.value(), kRecordHeaderSize);
    ASSERT_TRUE(stream->Flush().ok());

    EXPECT_TRUE(ReadRecordAt(begin.value(), RecordType::kTxnBegin).empty());
}

TEST_F(WalStreamTest, RecordsTooBigForASegmentOrTheRingAreRejected) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);

    // Records never span segments, so one that cannot fit a segment is a
    // design error, not something to split.
    const std::vector<std::byte> huge(stream->usable_segment_bytes());
    EXPECT_EQ(stream->Append(HeapInsert(1, 1), huge).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(stream->append_lsn(), kSegmentHeaderSize);
}

// ---- Backpressure --------------------------------------------------------

TEST_F(WalStreamTest, FullRingFailsWithOutOfSpaceAndDrainingClearsIt) {
    auto big_device = MemoryLogDevice::Create(1024 * 1024);
    ASSERT_TRUE(big_device.ok());
    auto opened = WalStream::Open(big_device.value().get(), 0, kMinRingCapacity);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto stream = std::move(opened.value());

    const std::vector<std::byte> payload = Pattern(kPayloadSize, 4);
    const std::size_t step = EncodedRecordSize(payload.size());

    Status last = Status::OK();
    std::size_t appended = 0;
    while (true) {
        auto lsn = stream->Append(HeapInsert(1, 1), payload);
        if (!lsn.ok()) {
            last = lsn.status();
            break;
        }
        ++appended;
        ASSERT_LT(appended, kMinRingCapacity / step + 2) << "ring never filled";
    }
    // Backpressure is a Status here; the reactor turns it into a suspension
    // when there is a task to suspend (wal.md section 6-4).
    EXPECT_EQ(last.code(), StatusCode::kOutOfSpace);
    EXPECT_LT(stream->ring_free(), step);

    ASSERT_TRUE(stream->Flush().ok());
    EXPECT_EQ(stream->ring_used(), 0u);
    EXPECT_TRUE(stream->Append(HeapInsert(1, 1), payload).ok());
}

// ---- Durability ----------------------------------------------------------

TEST_F(WalStreamTest, DurableLsnOnlyAdvancesOnSync) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 5);

    ASSERT_TRUE(stream->Append(HeapInsert(1, 1), payload).ok());
    ASSERT_TRUE(stream->Flush().ok());
    // Written is not durable: the gap between these two is exactly what a
    // crash takes.
    EXPECT_LT(stream->durable_lsn(), stream->flushed_lsn());

    ASSERT_TRUE(stream->Sync().ok());
    EXPECT_EQ(stream->durable_lsn(), stream->flushed_lsn());
}

TEST_F(WalStreamTest, FailedSyncDoesNotAdvanceTheDurablePoint) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    ASSERT_TRUE(stream->Append(HeapInsert(1, 1), Pattern(kPayloadSize, 6)).ok());

    const Lsn before = stream->durable_lsn();
    device_->FailNextSync(Status::IoError("injected"));
    EXPECT_EQ(stream->Sync().code(), StatusCode::kIoError);
    // A sync that failed proves nothing about what reached the platter.
    EXPECT_EQ(stream->durable_lsn(), before);

    ASSERT_TRUE(stream->Sync().ok());
    EXPECT_EQ(stream->durable_lsn(), stream->flushed_lsn());
}

TEST_F(WalStreamTest, FailedFlushKeepsTheBytesStagedForARetry) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 7);
    auto lsn = stream->Append(HeapInsert(3, 3), payload);
    ASSERT_TRUE(lsn.ok());

    device_->FailNextWrite(Status::IoError("injected"));
    EXPECT_EQ(stream->Flush().code(), StatusCode::kIoError);
    // Still staged, still unflushed - a retry writes the same range again.
    EXPECT_EQ(stream->ring_used(), EncodedRecordSize(payload.size()));
    EXPECT_EQ(stream->flushed_lsn(), kSegmentHeaderSize);

    ASSERT_TRUE(stream->Flush().ok());
    const auto read_back = ReadRecordAt(lsn.value(), RecordType::kHeapInsert);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), read_back.begin()));
}

TEST_F(WalStreamTest, CrashKeepsSyncedRecordsAndDropsTheRest) {
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 8);
    Lsn durable_end = 0;
    {
        auto stream = OpenStream();
        ASSERT_NE(stream, nullptr);
        ASSERT_TRUE(stream->Append(HeapInsert(1, 1), payload).ok());
        ASSERT_TRUE(stream->Append(HeapInsert(2, 2), payload).ok());
        ASSERT_TRUE(stream->Sync().ok());
        durable_end = stream->durable_lsn();

        // Flushed but never synced, plus one still staged.
        ASSERT_TRUE(stream->Append(HeapInsert(3, 3), payload).ok());
        ASSERT_TRUE(stream->Flush().ok());
        ASSERT_TRUE(stream->Append(HeapInsert(4, 4), payload).ok());
    }
    device_->Crash();

    auto reopened = OpenStream();
    ASSERT_NE(reopened, nullptr);
    // Recovery resumes at the end of what was synced, and appends over the
    // records that did not survive.
    EXPECT_EQ(reopened->append_lsn(), durable_end);
}

// ---- Segment roll --------------------------------------------------------

TEST_F(WalStreamTest, FillingASegmentSealsItAndRollsToTheNext) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 9);
    const std::size_t step = EncodedRecordSize(payload.size());

    Lsn last_in_first_segment = 0;
    Lsn first_in_second_segment = 0;
    for (std::uint64_t txn = 1;; ++txn) {
        auto lsn = stream->Append(HeapInsert(txn, 1), payload);
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
        if (lsn.value() / kSegmentSize == 1) {
            first_in_second_segment = lsn.value();
            break;
        }
        last_in_first_segment = lsn.value();
        ASSERT_LT(txn, kSegmentSize / step + 4) << "segment never filled";
    }
    ASSERT_TRUE(stream->Sync().ok());

    EXPECT_EQ(device_->segment_count(), 2u);
    // The roll skips the tail that could not hold the record, and the new
    // segment's first record sits just past its header block.
    EXPECT_EQ(first_in_second_segment, kSegmentSize + kSegmentHeaderSize);
    EXPECT_LT(last_in_first_segment + step, first_in_second_segment);

    std::vector<std::byte> header(kSegmentHeaderSize);
    ASSERT_TRUE(device_->ReadAt(1, 0, header).ok());
    auto decoded = DecodeSegmentHeader(header);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().segment_no, 1u);
    EXPECT_EQ(decoded.value().start_lsn, kSegmentSize);

    // The sealed segment ends with a PAD marker where the next record would
    // not fit.
    const auto pad_payload = ReadRecordAt(last_in_first_segment + step, RecordType::kPad);
    EXPECT_TRUE(pad_payload.empty());
}

TEST_F(WalStreamTest, SealIsExplicitAndIdempotent) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);
    ASSERT_TRUE(stream->Append(HeapInsert(1, 1), Pattern(kPayloadSize, 10)).ok());

    ASSERT_TRUE(stream->Seal().ok());
    EXPECT_TRUE(stream->sealed());
    EXPECT_EQ(stream->ring_used(), 0u);
    const Lsn after_seal = stream->append_lsn();

    ASSERT_TRUE(stream->Seal().ok());
    EXPECT_EQ(stream->append_lsn(), after_seal);
    EXPECT_EQ(device_->segment_count(), 1u);

    // The next append is what actually rolls.
    auto lsn = stream->Append(HeapInsert(2, 2), Pattern(kPayloadSize, 11));
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_EQ(lsn.value(), kSegmentSize + kSegmentHeaderSize);
    EXPECT_EQ(device_->segment_count(), 2u);
    EXPECT_FALSE(stream->sealed());
}

// ---- Reopen --------------------------------------------------------------

TEST_F(WalStreamTest, ReopenResumesAtTheDurableEnd) {
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 12);
    Lsn end = 0;
    {
        auto stream = OpenStream();
        ASSERT_NE(stream, nullptr);
        for (std::uint64_t txn = 1; txn <= 4; ++txn) {
            ASSERT_TRUE(stream->Append(HeapInsert(txn, 1), payload).ok());
        }
        ASSERT_TRUE(stream->Sync().ok());
        end = stream->append_lsn();
    }

    auto reopened = OpenStream();
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->append_lsn(), end);
    EXPECT_EQ(reopened->durable_lsn(), end);
    EXPECT_EQ(device_->segment_count(), 1u);  // adopted, not recreated

    auto lsn = reopened->Append(HeapInsert(5, 1), payload);
    ASSERT_TRUE(lsn.ok());
    EXPECT_EQ(lsn.value(), end);
}

TEST_F(WalStreamTest, ReopenOnASealedSegmentRollsOnTheNextAppend) {
    {
        auto stream = OpenStream();
        ASSERT_NE(stream, nullptr);
        ASSERT_TRUE(stream->Append(HeapInsert(1, 1), Pattern(kPayloadSize, 13)).ok());
        ASSERT_TRUE(stream->Seal().ok());
        ASSERT_TRUE(stream->Sync().ok());
    }

    auto reopened = OpenStream();
    ASSERT_NE(reopened, nullptr);
    // The PAD marker means the segment ends there, whatever the bytes after
    // it decode to.
    EXPECT_TRUE(reopened->sealed());

    auto lsn = reopened->Append(HeapInsert(2, 2), Pattern(kPayloadSize, 14));
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_EQ(lsn.value(), kSegmentSize + kSegmentHeaderSize);
}

TEST_F(WalStreamTest, ReopenStopsBeforeATornRecord) {
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 15);
    Lsn second = 0;
    {
        auto stream = OpenStream();
        ASSERT_NE(stream, nullptr);
        ASSERT_TRUE(stream->Append(HeapInsert(1, 1), payload).ok());
        ASSERT_TRUE(stream->Sync().ok());

        auto lsn = stream->Append(HeapInsert(2, 2), payload);
        ASSERT_TRUE(lsn.ok());
        second = lsn.value();
        // The flush lands half-written, as an interrupted one would.
        device_->TearNextWrite(EncodedRecordSize(payload.size()) / 2);
        ASSERT_TRUE(stream->Sync().ok());
    }

    auto reopened = OpenStream();
    ASSERT_NE(reopened, nullptr);
    // The torn record is the end of the stream, and the next append
    // overwrites it.
    EXPECT_EQ(reopened->append_lsn(), second);
}

TEST_F(WalStreamTest, ReopenRefusesAnotherCoresSegment) {
    {
        auto stream = OpenStream(1);
        ASSERT_NE(stream, nullptr);
        ASSERT_TRUE(stream->Sync().ok());
    }
    // Appending past a header that names a different stream would
    // interleave two cores' logs in one file.
    auto opened = WalStream::Open(device_.get(), 2, kMinRingCapacity);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

TEST_F(WalStreamTest, ReopenRefusesAMissingSegmentHeader) {
    ASSERT_TRUE(device_->CreateSegment(0).ok());  // zeroed, never headered
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

// ---- Composition with the payload codecs ---------------------------------

TEST_F(WalStreamTest, ATransactionsRecordsReadBackInOrder) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);

    const std::vector<std::byte> tuple = Pattern(64, 16);
    std::vector<std::byte> payload(kHeapWriteFixedSize + tuple.size());
    HeapWritePayload fields{};
    fields.trx_id = 77;
    fields.undo_ptr = 0;
    fields.slot = 5;
    auto payload_size = EncodeHeapWrite(payload, fields, tuple);
    ASSERT_TRUE(payload_size.ok()) << payload_size.status().message();

    ASSERT_TRUE(stream->Append({RecordType::kTxnBegin, 77, kInvalidPageId, 0}).ok());
    ASSERT_TRUE(stream->Append(HeapInsert(77, 12), payload).ok());
    const Lsn commit_lsn =
        stream->Append({RecordType::kTxnCommit, 77, kInvalidPageId, 0}).value();
    ASSERT_TRUE(stream->Sync().ok());
    EXPECT_GE(stream->durable_lsn(), commit_lsn);

    std::vector<std::byte> body(kSegmentSize - kSegmentHeaderSize);
    ASSERT_TRUE(device_->ReadAt(0, kSegmentHeaderSize, body).ok());
    RecordReader reader(body, kSegmentHeaderSize);

    auto begin = reader.Next();
    ASSERT_TRUE(begin.has_value());
    EXPECT_EQ(begin->type(), RecordType::kTxnBegin);

    auto insert = reader.Next();
    ASSERT_TRUE(insert.has_value());
    EXPECT_EQ(insert->type(), RecordType::kHeapInsert);
    auto decoded = DecodeHeapWrite(insert->payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.trx_id, fields.trx_id);
    EXPECT_EQ(decoded.value().fields.slot, fields.slot);
    ASSERT_EQ(decoded.value().tuple.size(), tuple.size());
    EXPECT_TRUE(std::equal(tuple.begin(), tuple.end(), decoded.value().tuple.begin()));

    auto commit = reader.Next();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->type(), RecordType::kTxnCommit);
    EXPECT_EQ(commit->header.lsn, commit_lsn);

    EXPECT_FALSE(reader.Next().has_value());
}

// ---- The exact-fill boundary (bench/results-bulk-insert.md's wedge) -------
//
// An append that exactly fills a segment leaves append_lsn_ at offset 0 of
// the *next* segment - a position no record ever legitimately occupies,
// since every segment opens with its header block. SegmentRemaining() used
// to answer a full segment there: the roll was skipped, the next record
// was placed in a segment that had never been created, and every later
// Flush refused the boundary-spanning range, wedging the stream for good.
// These two tests fill a segment to the byte and then keep going, on the
// live stream and through a reopen.

// 64-byte records (32-byte payload): kSegmentSize and kSegmentHeaderSize
// are both multiples of 64, so the usable span fills with no remainder.
constexpr std::size_t kExactFillPayload = 64 - kRecordHeaderSize;

TEST_F(WalStreamTest, AnExactlyFullSegmentRollsInsteadOfWedging) {
    auto stream = OpenStream();
    ASSERT_NE(stream, nullptr);

    const std::uint64_t records = (kSegmentSize - kSegmentHeaderSize) / 64;
    const auto payload = Pattern(kExactFillPayload, 5);
    for (std::uint64_t i = 0; i < records; ++i) {
        auto lsn = stream->Append(HeapInsert(1, 7), payload);
        ASSERT_TRUE(lsn.ok()) << "record " << i << ": " << lsn.status().message();
        if (stream->ring_used() > kMinRingCapacity / 2) {
            ASSERT_TRUE(stream->Flush().ok());
        }
    }
    // To the byte: the append cursor sits exactly on the boundary.
    EXPECT_EQ(stream->append_lsn(), kSegmentSize);

    // The wedge probe. Pre-fix this append believed a full segment
    // remained, skipped the roll, and the stream never recovered.
    auto rolled = stream->Append(HeapInsert(1, 7), payload);
    ASSERT_TRUE(rolled.ok()) << rolled.status().message();
    EXPECT_EQ(rolled.value(), kSegmentSize + kSegmentHeaderSize)
        << "the record must open segment 1, past its header";
    EXPECT_EQ(device_->segment_count(), 2u);
    ASSERT_TRUE(stream->Flush().ok());

    // The record is really there, at its address.
    EXPECT_EQ(ReadRecordAt(rolled.value(), RecordType::kHeapInsert), payload);
}

TEST_F(WalStreamTest, ReopeningAtAnExactlyFullSegmentRollsToo) {
    const auto payload = Pattern(kExactFillPayload, 5);
    {
        auto stream = OpenStream();
        ASSERT_NE(stream, nullptr);
        const std::uint64_t records = (kSegmentSize - kSegmentHeaderSize) / 64;
        for (std::uint64_t i = 0; i < records; ++i) {
            ASSERT_TRUE(stream->Append(HeapInsert(1, 7), payload).ok());
            if (stream->ring_used() > kMinRingCapacity / 2) {
                ASSERT_TRUE(stream->Flush().ok());
            }
        }
        ASSERT_TRUE(stream->Flush().ok());
        EXPECT_EQ(stream->append_lsn(), kSegmentSize);
    }

    // The mount path: ScanTail walks the exactly-full segment to its
    // boundary and the first append afterwards must roll, exactly as the
    // live stream did.
    auto reopened = OpenStream();
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->append_lsn(), kSegmentSize);

    auto lsn = reopened->Append(HeapInsert(2, 9), payload);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_EQ(lsn.value(), kSegmentSize + kSegmentHeaderSize);
    ASSERT_TRUE(reopened->Flush().ok());
    EXPECT_EQ(ReadRecordAt(lsn.value(), RecordType::kHeapInsert), payload);
}

}  // namespace
}  // namespace kds::wal
