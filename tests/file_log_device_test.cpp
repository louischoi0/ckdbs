#include "kds/wal/file_log_device.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/record.hpp"

// The on-disk half of the LogDevice contract. Every case that is not about
// files themselves is deliberately the same case memory_log_device_test.cpp
// asserts, because a simulator is only useful if it answers identically.

namespace kds::wal {
namespace {

// Small enough to keep the tests fast, big enough to hold the 4 KiB segment
// header plus records after it.
constexpr std::uint64_t kSmallSegment = 16384;

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
    return bytes;
}

class FileLogDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = (std::filesystem::temp_directory_path() /
                ("kds_file_log_device_" + std::string(info->name()) + "_" +
                 std::to_string(::getpid())))
                   .string();
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::unique_ptr<FileLogDevice> OpenDevice(std::uint32_t core_id = 0,
                                              std::uint64_t segment_size = kSmallSegment) {
        auto opened = FileLogDevice::Open(dir_, core_id, segment_size);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        return opened.ok() ? std::move(opened.value()) : nullptr;
    }

    std::string dir_;
};

TEST_F(FileLogDeviceTest, OpenCreatesTheDirectoryAndAdoptsNothing) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_TRUE(std::filesystem::is_directory(dir_));
    EXPECT_EQ(device->end_segment(), 0u);
    EXPECT_EQ(device->segment_size(), kSmallSegment);
    EXPECT_EQ(device->core_id(), 0u);
}

TEST_F(FileLogDeviceTest, OpenRejectsZeroSegmentSize) {
    auto opened = FileLogDevice::Open(dir_, 0, 0);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(FileLogDeviceTest, SegmentFileIsNamedAndFullySizedAtCreation) {
    auto device = OpenDevice(3);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::string path = device->SegmentPath(0);
    EXPECT_EQ(std::filesystem::path(path).filename().string(), "wal-3-0.log");
    ASSERT_TRUE(std::filesystem::exists(path));
    // Full size up front, so an append cannot fail for space after its
    // record was already accepted.
    EXPECT_EQ(std::filesystem::file_size(path), kSmallSegment);
}

TEST_F(FileLogDeviceTest, SegmentsAreCreatedInOrder) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);

    EXPECT_EQ(device->CreateSegment(1).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(device->end_segment(), 0u);
    // A rejected creation leaves no file behind.
    EXPECT_FALSE(std::filesystem::exists(device->SegmentPath(1)));

    ASSERT_TRUE(device->CreateSegment(0).ok());
    ASSERT_TRUE(device->CreateSegment(1).ok());
    EXPECT_EQ(device->end_segment(), 2u);
    EXPECT_EQ(device->CreateSegment(1).code(), StatusCode::kInvalidArgument);
}

TEST_F(FileLogDeviceTest, WriteThenReadRoundTrips) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> written = Pattern(512, 3);
    ASSERT_TRUE(device->WriteAt(0, 64, written).ok());
    ASSERT_TRUE(device->Sync().ok());

    std::vector<std::byte> read(written.size());
    ASSERT_TRUE(device->ReadAt(0, 64, read).ok());
    EXPECT_EQ(read, written);
}

TEST_F(FileLogDeviceTest, NeverWrittenBytesReadAsZeroes) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::vector<std::byte> read(128, std::byte{0xAB});
    ASSERT_TRUE(device->ReadAt(0, 1024, read).ok());
    EXPECT_EQ(read, std::vector<std::byte>(128, std::byte{0}));
}

TEST_F(FileLogDeviceTest, RangesOutsideASegmentAreOutOfRange) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::vector<std::byte> buffer(16);
    EXPECT_EQ(device->WriteAt(1, 0, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->ReadAt(1, 0, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WriteAt(0, kSmallSegment - 8, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->ReadAt(0, kSmallSegment - 8, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WriteAt(0, UINT64_MAX - 4, buffer).code(), StatusCode::kOutOfRange);

    EXPECT_TRUE(device->WriteAt(0, kSmallSegment - buffer.size(), buffer).ok());
}

TEST_F(FileLogDeviceTest, ReopenAdoptsExistingSegmentsInOrder) {
    const std::vector<std::byte> written = Pattern(64, 5);
    {
        auto device = OpenDevice();
        ASSERT_NE(device, nullptr);
        ASSERT_TRUE(device->CreateSegment(0).ok());
        ASSERT_TRUE(device->CreateSegment(1).ok());
        ASSERT_TRUE(device->WriteAt(1, 128, written).ok());
        ASSERT_TRUE(device->Sync().ok());
    }

    // This is how recovery finds the stream after a restart.
    auto reopened = OpenDevice();
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->end_segment(), 2u);

    std::vector<std::byte> read(written.size());
    ASSERT_TRUE(reopened->ReadAt(1, 128, read).ok());
    EXPECT_EQ(read, written);

    // Adoption continues the numbering rather than restarting it.
    ASSERT_TRUE(reopened->CreateSegment(2).ok());
}

TEST_F(FileLogDeviceTest, ReopenIgnoresOtherCoresStreams) {
    {
        auto core0 = OpenDevice(0);
        ASSERT_NE(core0, nullptr);
        ASSERT_TRUE(core0->CreateSegment(0).ok());

        auto core1 = OpenDevice(1);
        ASSERT_NE(core1, nullptr);
        ASSERT_TRUE(core1->CreateSegment(0).ok());
        ASSERT_TRUE(core1->CreateSegment(1).ok());
    }

    // Per-core streams share a directory but never each other's segments
    // (wal.md section 3).
    auto core0 = OpenDevice(0);
    ASSERT_NE(core0, nullptr);
    EXPECT_EQ(core0->end_segment(), 1u);

    auto core1 = OpenDevice(1);
    ASSERT_NE(core1, nullptr);
    EXPECT_EQ(core1->end_segment(), 2u);
}

TEST_F(FileLogDeviceTest, ReopenIgnoresForeignFiles) {
    {
        auto device = OpenDevice();
        ASSERT_NE(device, nullptr);
        ASSERT_TRUE(device->CreateSegment(0).ok());
    }
    // Archives, temp files, and misspelled numbers are not this stream's
    // segments and must not be adopted as one.
    for (const char* name : {"wal-0-0.log.archived", "notes.txt", "wal-0-.log", "wal-0-01.log",
                             "wal-0-x.log"}) {
        std::ofstream(dir_ + "/" + name) << "x";
    }

    auto reopened = OpenDevice();
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->end_segment(), 1u);
}

TEST_F(FileLogDeviceTest, GapInTheNumberingIsCorruption) {
    {
        auto device = OpenDevice();
        ASSERT_NE(device, nullptr);
        ASSERT_TRUE(device->CreateSegment(0).ok());
        ASSERT_TRUE(device->CreateSegment(1).ok());
        ASSERT_TRUE(device->CreateSegment(2).ok());
    }
    std::filesystem::remove(dir_ + "/wal-0-1.log");

    auto opened = FileLogDevice::Open(dir_, 0, kSmallSegment);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

TEST_F(FileLogDeviceTest, WrongSizedSegmentIsCorruption) {
    {
        auto device = OpenDevice();
        ASSERT_NE(device, nullptr);
        ASSERT_TRUE(device->CreateSegment(0).ok());
    }
    std::filesystem::resize_file(dir_ + "/wal-0-0.log", kSmallSegment / 2);

    // Truncated, or written under a different segment_size - either way the
    // LSN-to-offset arithmetic no longer holds.
    auto opened = FileLogDevice::Open(dir_, 0, kSmallSegment);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);

    auto mismatched = FileLogDevice::Open(dir_, 0, kSmallSegment * 2);
    ASSERT_FALSE(mismatched.ok());
    EXPECT_EQ(mismatched.status().code(), StatusCode::kCorruption);
}

TEST_F(FileLogDeviceTest, CreatingOverAnUnadoptedFileIsRefused) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    // Appeared behind the device's back; overwriting it would silently
    // discard whatever it holds.
    std::ofstream(device->SegmentPath(1)) << "x";
    EXPECT_EQ(device->CreateSegment(1).code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(device->end_segment(), 1u);
}

TEST_F(FileLogDeviceTest, StreamWrittenThroughTheDeviceReadsBackAsRecords) {
    // The composition that matters: a segment header plus records go down
    // through the device, and record.hpp's reader walks them back up.
    auto device = OpenDevice(2);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::vector<std::byte> header(kSegmentHeaderSize);
    SegmentHeaderFields fields{};
    fields.magic = kSegmentMagic;
    fields.format_version = kSegmentFormatVersion;
    fields.core_id = 2;
    fields.segment_no = 0;
    fields.start_lsn = 0;
    ASSERT_TRUE(EncodeSegmentHeader(header, fields).ok());
    ASSERT_TRUE(device->WriteAt(0, 0, header).ok());

    const std::vector<std::byte> payload = Pattern(40, 6);
    std::vector<std::byte> records(256);
    Lsn lsn = kSegmentHeaderSize;
    std::size_t used = 0;
    for (std::uint64_t txn = 1; txn <= 3; ++txn) {
        const RecordSpec spec{RecordType::kHeapInsert, txn, static_cast<PageId>(txn), 0};
        auto encoded = EncodeRecord(std::span(records).subspan(used), spec, lsn, payload);
        ASSERT_TRUE(encoded.ok()) << encoded.status().message();
        used += encoded.value();
        lsn += encoded.value();
    }
    ASSERT_TRUE(device->WriteAt(0, kSegmentHeaderSize, std::span(records).first(used)).ok());
    ASSERT_TRUE(device->Sync().ok());

    // Read back through a fresh device, as recovery would.
    auto reopened = OpenDevice(2);
    ASSERT_NE(reopened, nullptr);

    std::vector<std::byte> header_read(kSegmentHeaderSize);
    ASSERT_TRUE(reopened->ReadAt(0, 0, header_read).ok());
    auto decoded_header = DecodeSegmentHeader(header_read);
    ASSERT_TRUE(decoded_header.ok()) << decoded_header.status().message();
    EXPECT_EQ(decoded_header.value().core_id, 2u);
    EXPECT_EQ(decoded_header.value().start_lsn, 0u);

    std::vector<std::byte> body(used);
    ASSERT_TRUE(reopened->ReadAt(0, kSegmentHeaderSize, body).ok());
    RecordReader reader(body, kSegmentHeaderSize);
    for (std::uint64_t txn = 1; txn <= 3; ++txn) {
        auto record = reader.Next();
        ASSERT_TRUE(record.has_value()) << "record " << txn;
        EXPECT_EQ(record->header.txn_id, txn);
        EXPECT_EQ(record->header.page_id, static_cast<PageId>(txn));
        EXPECT_EQ(record->type(), RecordType::kHeapInsert);
        EXPECT_TRUE(std::equal(payload.begin(), payload.end(), record->payload.begin()));
    }
    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_FALSE(reader.stopped_early());
}

TEST_F(FileLogDeviceTest, UnwrittenTailOfASegmentEndsTheStreamCleanly) {
    // Segments are created at full size, so the bytes past the last record
    // read as zeroes - which must look like the end of the stream, not a
    // record with total_len 0 that the reader tries to skip over.
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> payload = Pattern(8, 1);
    std::vector<std::byte> encoded(64);
    auto size = EncodeRecord(encoded, {RecordType::kTxnCommit, 9, kInvalidPageId, 0},
                             kSegmentHeaderSize, payload);
    ASSERT_TRUE(size.ok()) << size.status().message();
    ASSERT_TRUE(device->WriteAt(0, kSegmentHeaderSize, std::span(encoded).first(size.value())).ok());

    std::vector<std::byte> body(kSmallSegment - kSegmentHeaderSize);
    ASSERT_TRUE(device->ReadAt(0, kSegmentHeaderSize, body).ok());

    RecordReader reader(body, kSegmentHeaderSize);
    ASSERT_TRUE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_EQ(reader.end_lsn(), kSegmentHeaderSize + size.value());
}

// ---- Recycling (BC-S2, `workorder-bc-wal-recycling.md` BC-R3, BC-R4) ------

// The segment files present in the directory, by number.
std::vector<std::uint64_t> SegmentsOnDisk(const std::string& dir) {
    std::vector<std::uint64_t> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("wal-0-", 0) != 0) continue;
        out.push_back(std::stoull(name.substr(6)));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Creates segments 0..n-1 through a device, then lets it go.
void MakeSegments(const std::string& dir, std::uint64_t n) {
    auto opened = FileLogDevice::Open(dir, 0, kSmallSegment);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    for (std::uint64_t s = 0; s < n; ++s) ASSERT_TRUE(opened.value()->CreateSegment(s).ok());
}

TEST_F(FileLogDeviceTest, DetachLeavesTheFilesUntilReclaimAndReclaimRemovesThem) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    for (std::uint64_t s = 0; s < 4; ++s) ASSERT_TRUE(device->CreateSegment(s).ok());

    ASSERT_TRUE(device->DetachBelow(2).ok());
    EXPECT_EQ(device->first_segment(), 2u);
    EXPECT_EQ(device->end_segment(), 4u);
    std::vector<std::byte> buf(16);
    EXPECT_EQ(device->ReadAt(1, kSegmentHeaderSize, buf).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WriteAt(0, kSegmentHeaderSize, buf).code(), StatusCode::kOutOfRange);
    EXPECT_TRUE(device->ReadAt(2, kSegmentHeaderSize, buf).ok());
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{0, 1, 2, 3}))
        << "a detach does no I/O";

    ASSERT_TRUE(device->ReclaimDetached().ok());
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{2, 3}));
    EXPECT_EQ(device->segments_removed(), 2u);
    ASSERT_TRUE(device->ReclaimDetached().ok()) << "an empty queue is a no-op";
    EXPECT_EQ(device->segments_removed(), 2u);

    // The run goes on from where it was: the next segment is 4, not 2.
    EXPECT_EQ(device->CreateSegment(2).code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(device->CreateSegment(4).ok());
}

TEST_F(FileLogDeviceTest, DetachNeverReachesTheLastSegment) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->DetachBelow(0).code(), StatusCode::kInvalidArgument) << "empty device";
    for (std::uint64_t s = 0; s < 3; ++s) ASSERT_TRUE(device->CreateSegment(s).ok());
    EXPECT_EQ(device->DetachBelow(3).code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(device->DetachBelow(0).ok()) << "at the first: detaches nothing";
    EXPECT_TRUE(device->DetachBelow(2).ok()) << "everything but the last";
    EXPECT_EQ(device->DetachBelow(1).code(), StatusCode::kInvalidArgument) << "below the first";
    EXPECT_EQ(device->first_segment(), 2u);
}

TEST_F(FileLogDeviceTest, OpenSkipsLeftoversBelowTheAnchorAndDeletesNone) {
    // Segments 0..5, then 1 and 3 removed as a crash mid-reclaim would leave
    // them: a subset of the detached names back, with gaps.
    MakeSegments(dir_, 6);
    std::filesystem::remove(dir_ + "/wal-0-1.log");
    std::filesystem::remove(dir_ + "/wal-0-3.log");

    auto opened = FileLogDevice::Open(dir_, 0, kSmallSegment, /*first_needed=*/4);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    FileLogDevice& device = *opened.value();
    EXPECT_EQ(device.first_segment(), 4u);
    EXPECT_EQ(device.end_segment(), 6u);
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{0, 2, 4, 5}))
        << "a mount may still be refused after Open; it deletes nothing";

    // The leftovers go with the next reclaim, not before.
    ASSERT_TRUE(device.ReclaimDetached().ok());
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{4, 5}));
}

TEST_F(FileLogDeviceTest, AGapAboveTheAnchorIsCorruption) {
    MakeSegments(dir_, 6);
    std::filesystem::remove(dir_ + "/wal-0-4.log");
    auto opened = FileLogDevice::Open(dir_, 0, kSmallSegment, /*first_needed=*/3);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
    EXPECT_NE(opened.status().message().find("segment 4 is missing"), std::string::npos)
        << opened.status().message();
}

TEST_F(FileLogDeviceTest, TheAnchorsSegmentMissingIsCorruption) {
    MakeSegments(dir_, 6);
    std::filesystem::remove(dir_ + "/wal-0-3.log");
    auto opened = FileLogDevice::Open(dir_, 0, kSmallSegment, /*first_needed=*/3);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

TEST_F(FileLogDeviceTest, NoSegmentAtOrAboveTheAnchorIsCorruptionAndDeletesNothing) {
    MakeSegments(dir_, 2);
    auto below = FileLogDevice::Open(dir_, 0, kSmallSegment, /*first_needed=*/3);
    ASSERT_FALSE(below.ok());
    EXPECT_EQ(below.status().code(), StatusCode::kCorruption);
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{0, 1}));

    std::filesystem::remove_all(dir_);
    auto empty = FileLogDevice::Open(dir_, 0, kSmallSegment, /*first_needed=*/3);
    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(empty.status().code(), StatusCode::kCorruption);
}

// BC-R4: a sync that copied a segment's descriptor keeps it open while a
// detach and a reclaim take the segment away on another thread. Before
// BC-S2 `Sync` copied raw numbers, and a descriptor closed under it failed
// the sync or synced whatever file reused the number.
//
// The interleaving is made likely rather than lucky. Each round detaches two
// segments at once, the older holding dirty data, so a sync that copied both
// is inside the older one's `fdatasync` when the younger one is detached.
// The pipe opened right after takes the lowest free descriptor number - the
// younger segment's, if the detach closed it - which is what a process's
// next socket would do; `fdatasync` on a pipe fails, so a sync that kept
// only the number reports it.
TEST_F(FileLogDeviceTest, ASyncBesideDetachAndReclaimNeverFails) {
    constexpr std::uint64_t kSegment = 1 << 20;
    auto device = OpenDevice(0, kSegment);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::atomic<bool> stop{false};
    std::atomic<int> syncs{0};
    std::atomic<int> failures{0};
    std::thread syncer([&] {
        while (!stop.load()) {
            if (!device->Sync().ok()) failures.fetch_add(1);
            syncs.fetch_add(1);
        }
    });
    // An ASSERT below returns early; a still-joinable thread would then
    // std::terminate the whole binary instead of reporting the failure.
    struct StopAndJoin {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~StopAndJoin() {
            stop.store(true);
            if (thread.joinable()) thread.join();
        }
    } stop_and_join{stop, syncer};

    // This thread is the stream: it alone creates, writes and detaches,
    // which is the serialization the contract asks of the caller.
    const std::vector<std::byte> dirty = Pattern(kSegment / 2, 3);
    for (int round = 0; round < 40; ++round) {
        const std::uint64_t older = device->end_segment() - 1;
        ASSERT_TRUE(device->CreateSegment(older + 1).ok());
        ASSERT_TRUE(device->CreateSegment(older + 2).ok());
        ASSERT_TRUE(device->WriteAt(older, kSegmentHeaderSize, dirty).ok());
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        ASSERT_TRUE(device->DetachBelow(older + 2).ok());
        int squatter[2];
        ASSERT_EQ(::pipe(squatter), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ASSERT_TRUE(device->ReclaimDetached().ok());
        ::close(squatter[0]);
        ::close(squatter[1]);
    }
    stop.store(true);
    syncer.join();

    EXPECT_EQ(failures.load(), 0) << "of " << syncs.load() << " syncs";
    EXPECT_GT(syncs.load(), 0);
    EXPECT_EQ(SegmentsOnDisk(dir_), (std::vector<std::uint64_t>{80}));
    EXPECT_EQ(device->segments_removed(), 80u);
}

// The directory descriptor is opened once, at `Open`: a roll that meets the
// descriptor limit fails at the segment's own open and leaves no file
// behind, where the per-roll directory open stranded a full-size headerless
// one (the bug entry, deleted with the fix, is at `19e1dc1f`).
TEST_F(FileLogDeviceTest, ARollAtTheDescriptorLimitStrandsNoFile) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);

    struct ::rlimit saved {};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
    const int probe = ::dup(0);
    ASSERT_GE(probe, 0);
    ::close(probe);
    struct ::rlimit low = saved;
    low.rlim_cur = static_cast<rlim_t>(probe + 8);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &low), 0);

    Status failed = Status::OK();
    for (std::uint64_t s = 0; s < 64 && failed.ok(); ++s) failed = device->CreateSegment(s);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);

    ASSERT_FALSE(failed.ok()) << "the limit was never met";
    EXPECT_EQ(SegmentsOnDisk(dir_).size(), device->end_segment())
        << "the failed roll left a file the device does not hold: " << failed.message();
    EXPECT_TRUE(device->CreateSegment(device->end_segment()).ok())
        << "the next roll, with descriptors free again, is not blocked by a stranded name";
}

}  // namespace
}  // namespace kds::wal
