#include "kds/wal/memory_log_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/record.hpp"

// The simulator half of the LogDevice contract (wal.md section 16). The
// contract cases here are deliberately mirrored in file_log_device_test.cpp:
// a crash-consistency test is only worth writing against MemoryLogDevice if
// the two devices answer identically for identical arguments.

namespace kds::wal {
namespace {

// Big enough to hold the 4 KiB segment header plus records after it.
constexpr std::uint64_t kSmallSegment = 16384;

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
    return bytes;
}

std::unique_ptr<MemoryLogDevice> MakeDevice(std::uint64_t segment_size = kSmallSegment) {
    auto created = MemoryLogDevice::Create(segment_size);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.ok() ? std::move(created.value()) : nullptr;
}

TEST(MemoryLogDeviceTest, CreateRejectsZeroSegmentSize) {
    auto created = MemoryLogDevice::Create(0);
    ASSERT_FALSE(created.ok());
    EXPECT_EQ(created.status().code(), StatusCode::kInvalidArgument);
}

TEST(MemoryLogDeviceTest, SegmentsAreCreatedInOrder) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->segment_count(), 0u);

    // Out of order is rejected, and rejection does not create anything.
    EXPECT_EQ(device->CreateSegment(1).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(device->segment_count(), 0u);

    ASSERT_TRUE(device->CreateSegment(0).ok());
    ASSERT_TRUE(device->CreateSegment(1).ok());
    EXPECT_EQ(device->segment_count(), 2u);
    EXPECT_EQ(device->CreateSegment(1).code(), StatusCode::kInvalidArgument);
}

TEST(MemoryLogDeviceTest, WriteThenReadRoundTrips) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> written = Pattern(512, 3);
    ASSERT_TRUE(device->WriteAt(0, 64, written).ok());

    std::vector<std::byte> read(written.size());
    ASSERT_TRUE(device->ReadAt(0, 64, read).ok());
    EXPECT_EQ(read, written);
}

TEST(MemoryLogDeviceTest, NeverWrittenBytesReadAsZeroes) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::vector<std::byte> read(128, std::byte{0xAB});
    ASSERT_TRUE(device->ReadAt(0, 1024, read).ok());
    EXPECT_EQ(read, std::vector<std::byte>(128, std::byte{0}));
}

TEST(MemoryLogDeviceTest, SegmentsAreIndependent) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());
    ASSERT_TRUE(device->CreateSegment(1).ok());

    const std::vector<std::byte> written = Pattern(64, 9);
    ASSERT_TRUE(device->WriteAt(1, 0, written).ok());

    std::vector<std::byte> read(64, std::byte{0xFF});
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, std::vector<std::byte>(64, std::byte{0}));
}

TEST(MemoryLogDeviceTest, RangesOutsideASegmentAreOutOfRange) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    std::vector<std::byte> buffer(16);
    EXPECT_EQ(device->WriteAt(1, 0, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->ReadAt(1, 0, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WriteAt(0, kSmallSegment - 8, buffer).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->ReadAt(0, kSmallSegment - 8, buffer).code(), StatusCode::kOutOfRange);

    // A range that would wrap u64 must not be read as "starts at zero".
    EXPECT_EQ(device->WriteAt(0, UINT64_MAX - 4, buffer).code(), StatusCode::kOutOfRange);

    // Exactly filling the segment tail is legal, one byte more is not.
    EXPECT_TRUE(device->WriteAt(0, kSmallSegment - buffer.size(), buffer).ok());
}

TEST(MemoryLogDeviceTest, CrashDropsEverythingSinceTheLastSync) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> durable = Pattern(32, 1);
    ASSERT_TRUE(device->WriteAt(0, 0, durable).ok());
    ASSERT_TRUE(device->Sync().ok());

    const std::vector<std::byte> pending = Pattern(32, 2);
    ASSERT_TRUE(device->WriteAt(0, 128, pending).ok());
    ASSERT_TRUE(device->CreateSegment(1).ok());
    ASSERT_TRUE(device->WriteAt(1, 0, pending).ok());

    device->Crash();

    // The synced write survives; the ones after it, and the segment created
    // after it, do not.
    EXPECT_EQ(device->segment_count(), 1u);
    std::vector<std::byte> read(durable.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, durable);

    std::vector<std::byte> after(pending.size());
    ASSERT_TRUE(device->ReadAt(0, 128, after).ok());
    EXPECT_EQ(after, std::vector<std::byte>(pending.size(), std::byte{0}));
}

TEST(MemoryLogDeviceTest, ReadsSeeUndurableWritesUntilTheCrash) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> pending = Pattern(16, 5);
    ASSERT_TRUE(device->WriteAt(0, 0, pending).ok());

    // Durability is what Sync() buys; visibility is not (log_device.hpp).
    std::vector<std::byte> read(pending.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, pending);
}

TEST(MemoryLogDeviceTest, FailedSyncLeavesTheDurableImageBehind) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());
    ASSERT_TRUE(device->Sync().ok());

    const std::vector<std::byte> pending = Pattern(16, 6);
    ASSERT_TRUE(device->WriteAt(0, 0, pending).ok());

    // A Sync() that fails advances nothing: the write is exactly as
    // undurable afterwards as it was before, which is what lets a caller
    // treat a failed flush as "the durable LSN did not move".
    device->FailNextSync(Status::IoError("injected"));
    EXPECT_EQ(device->Sync().code(), StatusCode::kIoError);

    device->Crash();
    EXPECT_EQ(device->segment_count(), 1u);
    std::vector<std::byte> read(pending.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, std::vector<std::byte>(pending.size(), std::byte{0}));
}

TEST(MemoryLogDeviceTest, InjectedFailuresAreOneShot) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> bytes = Pattern(16, 7);
    device->FailNextWrite(Status::IoError("injected"));
    EXPECT_EQ(device->WriteAt(0, 0, bytes).code(), StatusCode::kIoError);

    // The failed write had no effect, and the next one is not affected.
    std::vector<std::byte> read(bytes.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, std::vector<std::byte>(bytes.size(), std::byte{0}));

    ASSERT_TRUE(device->WriteAt(0, 0, bytes).ok());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    EXPECT_EQ(read, bytes);
}

TEST(MemoryLogDeviceTest, TornWriteTransfersOnlyItsPrefixAndReportsOk) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> bytes = Pattern(64, 8);
    device->TearNextWrite(20);
    // A torn write is not an error the device knows about - that is exactly
    // why records carry a CRC (record.hpp).
    ASSERT_TRUE(device->WriteAt(0, 0, bytes).ok());

    std::vector<std::byte> read(bytes.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(read[i], i < 20 ? bytes[i] : std::byte{0}) << "at byte " << i;
    }
}

TEST(MemoryLogDeviceTest, TornRecordIsWhatTheReaderCallsTheEndOfTheStream) {
    // The composition the crash matrix is built on: the device tears a
    // write, and record.hpp's reader stops there rather than trusting it.
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> payload = Pattern(24, 4);
    std::array<std::byte, 256> buffer{};
    const RecordSpec spec{RecordType::kHeapInsert, 7, 42, 0};
    auto first = EncodeRecord(buffer, spec, kSegmentHeaderSize, payload);
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto second = EncodeRecord(std::span(buffer).subspan(first.value()), spec,
                               kSegmentHeaderSize + first.value(), payload);
    ASSERT_TRUE(second.ok()) << second.status().message();
    const std::size_t total = first.value() + second.value();

    // The second record lands half-written, as an interrupted flush would.
    ASSERT_TRUE(device->WriteAt(0, kSegmentHeaderSize, std::span(buffer).first(first.value())).ok());
    device->TearNextWrite(second.value() / 2);
    ASSERT_TRUE(device->WriteAt(0, kSegmentHeaderSize + first.value(),
                                std::span(buffer).subspan(first.value(), second.value()))
                    .ok());

    std::vector<std::byte> read(total);
    ASSERT_TRUE(device->ReadAt(0, kSegmentHeaderSize, read).ok());

    RecordReader reader(read, kSegmentHeaderSize);
    ASSERT_TRUE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_TRUE(reader.stopped_early());
    EXPECT_EQ(reader.end_lsn(), kSegmentHeaderSize + first.value());
}

TEST(MemoryLogDeviceTest, StatsAndTraceRecordWhatTheDeviceWasAsked) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->CreateSegment(0).ok());

    const std::vector<std::byte> bytes = Pattern(32, 2);
    ASSERT_TRUE(device->WriteAt(0, 0, bytes).ok());
    std::vector<std::byte> read(bytes.size());
    ASSERT_TRUE(device->ReadAt(0, 0, read).ok());
    ASSERT_TRUE(device->Sync().ok());

    EXPECT_EQ(device->stats().segments_created, 1u);
    EXPECT_EQ(device->stats().writes, 1u);
    EXPECT_EQ(device->stats().reads, 1u);
    EXPECT_EQ(device->stats().syncs, 1u);
    EXPECT_EQ(device->stats().bytes_written, bytes.size());

    // Ordering assertions (wal.md section 8) read this trace, so the write
    // must be recorded before the sync that made it durable.
    ASSERT_EQ(device->trace().size(), 4u);
    EXPECT_EQ(device->trace()[0].kind, MemoryLogDevice::OpKind::kCreate);
    EXPECT_EQ(device->trace()[1].kind, MemoryLogDevice::OpKind::kWrite);
    EXPECT_EQ(device->trace()[2].kind, MemoryLogDevice::OpKind::kRead);
    EXPECT_EQ(device->trace()[3].kind, MemoryLogDevice::OpKind::kSync);
    EXPECT_EQ(device->trace()[1].length, bytes.size());

    device->ClearTrace();
    EXPECT_TRUE(device->trace().empty());
}

}  // namespace
}  // namespace kds::wal
