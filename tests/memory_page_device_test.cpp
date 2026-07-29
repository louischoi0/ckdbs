#include "kds/storage/memory_page_device.hpp"

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/page_header.hpp"

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> Mut(Page& p) { return std::span<std::byte, kPageSize>(p); }
std::span<const std::byte, kPageSize> Const(const Page& p) {
    return std::span<const std::byte, kPageSize>(p);
}

Page PatternPage(std::uint8_t seed) {
    Page page{};
    for (std::size_t i = 0; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
    return page;
}

std::unique_ptr<MemoryPageDevice> MakeDevice(std::uint32_t extent_pages = 8,
                                             std::uint32_t initial_pages = 8) {
    auto created = MemoryPageDevice::Create(extent_pages, initial_pages);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.ok() ? std::move(created.value()) : nullptr;
}

TEST(MemoryPageDeviceTest, CreateRejectsZeroExtentAndOverCeiling) {
    EXPECT_EQ(MemoryPageDevice::Create(0).status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(MemoryPageDevice::Create(8, kMaxPageCount + 1).status().code(),
              StatusCode::kInvalidArgument);
}

TEST(MemoryPageDeviceTest, InitialPagesRoundUpLikeGrowthWould) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/9);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->page_capacity(), 16u);
    EXPECT_EQ(device->durable_page_capacity(), 16u);
}

// The behaviours below must match FilePageDevice exactly - that is what
// makes the simulated device a valid stand-in.
TEST(MemoryPageDeviceTest, WriteThenReadRoundTrips) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page written = PatternPage(5);
    ASSERT_TRUE(device->WritePage(3, Const(written)).ok());

    Page read{};
    ASSERT_TRUE(device->ReadPage(3, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), written.data(), kPageSize), 0);
}

TEST(MemoryPageDeviceTest, UnwrittenPageReadsZeroes) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    Page read{};
    std::memset(read.data(), 0xFF, read.size());
    ASSERT_TRUE(device->ReadPage(7, Mut(read)).ok());
    for (std::size_t i = 0; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "byte " << i;
    }
}

TEST(MemoryPageDeviceTest, AccessBeyondCapacityIsOutOfRange) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_NE(device, nullptr);

    Page page{};
    EXPECT_EQ(device->ReadPage(8, Mut(page)).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WritePage(8, Const(page)).code(), StatusCode::kOutOfRange);
}

TEST(MemoryPageDeviceTest, GrowthRoundsUpAndNeverShrinks) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->page_capacity(), 0u);

    ASSERT_TRUE(device->EnsureCapacity(1).ok());
    EXPECT_EQ(device->page_capacity(), 8u);

    ASSERT_TRUE(device->EnsureCapacity(8).ok());  // idempotent
    EXPECT_EQ(device->page_capacity(), 8u);
    EXPECT_EQ(device->stats().grows, 1u);

    ASSERT_TRUE(device->EnsureCapacity(1).ok());  // never shrinks
    EXPECT_EQ(device->page_capacity(), 8u);

    EXPECT_EQ(device->EnsureCapacity(kMaxPageCount + 1).code(), StatusCode::kInvalidArgument);
}

// ---- Durability model ---------------------------------------------------

TEST(MemoryPageDeviceTest, CrashDiscardsUnsyncedWrites) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page durable = PatternPage(1);
    ASSERT_TRUE(device->WritePage(2, Const(durable)).ok());
    ASSERT_TRUE(device->Sync().ok());

    const Page lost = PatternPage(2);
    ASSERT_TRUE(device->WritePage(2, Const(lost)).ok());

    // Visible before the crash: a write is readable immediately, it is just
    // not durable.
    Page read{};
    ASSERT_TRUE(device->ReadPage(2, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), lost.data(), kPageSize), 0);

    device->Crash();

    ASSERT_TRUE(device->ReadPage(2, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), durable.data(), kPageSize), 0);
}

// page.md section 14: growth is only durable once synced, which is exactly
// why the ALLOC record must be logged before the extension.
TEST(MemoryPageDeviceTest, CrashRevertsUnsyncedGrowth) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->Sync().ok());

    ASSERT_TRUE(device->EnsureCapacity(9).ok());
    EXPECT_EQ(device->page_capacity(), 16u);

    device->Crash();
    EXPECT_EQ(device->page_capacity(), 8u);

    Page page{};
    EXPECT_EQ(device->ReadPage(8, Mut(page)).code(), StatusCode::kOutOfRange);
}

TEST(MemoryPageDeviceTest, SyncedWritesSurviveCrash) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    for (PageId id = 0; id < 4; ++id) {
        const Page page = PatternPage(static_cast<std::uint8_t>(id + 1));
        ASSERT_TRUE(device->WritePage(id, Const(page)).ok());
    }
    ASSERT_TRUE(device->Sync().ok());
    device->Crash();

    for (PageId id = 0; id < 4; ++id) {
        const Page expected = PatternPage(static_cast<std::uint8_t>(id + 1));
        Page read{};
        ASSERT_TRUE(device->ReadPage(id, Mut(read)).ok());
        EXPECT_EQ(std::memcmp(read.data(), expected.data(), kPageSize), 0) << "page " << id;
    }
}

// ---- Torn writes --------------------------------------------------------

TEST(MemoryPageDeviceTest, TornWriteKeepsPrefixAndReportsSuccess) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page original = PatternPage(1);
    ASSERT_TRUE(device->WritePage(0, Const(original)).ok());
    ASSERT_TRUE(device->Sync().ok());

    const Page replacement = PatternPage(2);
    device->TearNextWrite(512);
    // A torn write is not an error the device can report - that is the
    // whole reason the page carries a checksum.
    ASSERT_TRUE(device->WritePage(0, Const(replacement)).ok());

    Page read{};
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), replacement.data(), 512), 0);
    EXPECT_EQ(std::memcmp(read.data() + 512, original.data() + 512, kPageSize - 512), 0);
}

TEST(MemoryPageDeviceTest, TornWriteIsOneShot) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page page = PatternPage(3);
    device->TearNextWrite(64);
    ASSERT_TRUE(device->WritePage(1, Const(page)).ok());
    ASSERT_TRUE(device->WritePage(1, Const(page)).ok());  // whole page this time

    Page read{};
    ASSERT_TRUE(device->ReadPage(1, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), page.data(), kPageSize), 0);
}

TEST(MemoryPageDeviceTest, TornRunWriteStopsMidRun) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    constexpr std::uint32_t kRun = 3;
    std::vector<std::byte> batch(kRun * kPageSize);
    for (std::uint32_t i = 0; i < kRun; ++i) {
        const Page page = PatternPage(static_cast<std::uint8_t>(i + 20));
        std::memcpy(batch.data() + i * kPageSize, page.data(), kPageSize);
    }

    // One and a half pages land; the third page never does.
    device->TearNextWrite(kPageSize + kPageSize / 2);
    ASSERT_TRUE(device->WritePageRun(0, kRun, batch).ok());

    Page read{};
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), batch.data(), kPageSize), 0);

    ASSERT_TRUE(device->ReadPage(1, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), batch.data() + kPageSize, kPageSize / 2), 0);
    for (std::size_t i = kPageSize / 2; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "page 1 byte " << i;
    }

    ASSERT_TRUE(device->ReadPage(2, Mut(read)).ok());
    for (std::size_t i = 0; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "page 2 byte " << i;
    }
}

// The checksum's reason for existing: a torn page is detected on load.
TEST(MemoryPageDeviceTest, ChecksumCatchesTornPage) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    Page first{};
    FormatPage(Mut(first), PageType::kHeap);
    std::memset(first.data() + kPageBodyOffset, 0x11, kPageBodySize);
    StampPageChecksum(Mut(first));
    ASSERT_TRUE(device->WritePage(0, Const(first)).ok());
    ASSERT_TRUE(device->Sync().ok());

    Page second{};
    FormatPage(Mut(second), PageType::kHeap);
    std::memset(second.data() + kPageBodyOffset, 0x22, kPageBodySize);
    StampPageChecksum(Mut(second));

    device->TearNextWrite(4096);  // half the page updated, half stale
    ASSERT_TRUE(device->WritePage(0, Const(second)).ok());

    Page read{};
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    EXPECT_EQ(VerifyPageChecksum(Const(read)).code(), StatusCode::kCorruption);
}

// ---- Injected I/O errors ------------------------------------------------

TEST(MemoryPageDeviceTest, InjectedFailuresAreOneShotAndHaveNoEffect) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page page = PatternPage(4);

    device->FailNextWrite(Status::IoError("injected write failure"));
    EXPECT_EQ(device->WritePage(0, Const(page)).code(), StatusCode::kIoError);

    Page read{};
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    for (std::size_t i = 0; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "failed write must not land, byte " << i;
    }

    ASSERT_TRUE(device->WritePage(0, Const(page)).ok());

    device->FailNextRead(Status::Corruption("injected read failure"));
    EXPECT_EQ(device->ReadPage(0, Mut(read)).code(), StatusCode::kCorruption);
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), page.data(), kPageSize), 0);
}

TEST(MemoryPageDeviceTest, FailedSyncLeavesWritesUndurable) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);

    const Page page = PatternPage(6);
    ASSERT_TRUE(device->WritePage(0, Const(page)).ok());

    device->FailNextSync(Status::IoError("injected fsync failure"));
    EXPECT_EQ(device->Sync().code(), StatusCode::kIoError);

    device->Crash();
    Page read{};
    ASSERT_TRUE(device->ReadPage(0, Mut(read)).ok());
    for (std::size_t i = 0; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "byte " << i;
    }
}

TEST(MemoryPageDeviceTest, FailedGrowLeavesCapacityUnchanged) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_NE(device, nullptr);

    device->FailNextGrow(Status::OutOfSpace("injected ENOSPC"));
    EXPECT_EQ(device->EnsureCapacity(9).code(), StatusCode::kOutOfSpace);
    EXPECT_EQ(device->page_capacity(), 8u);

    ASSERT_TRUE(device->EnsureCapacity(9).ok());
    EXPECT_EQ(device->page_capacity(), 16u);
}

// ---- Instrumentation ----------------------------------------------------

// page.md section 18-7 wants flush batches asserted against the I/O trace,
// which requires a coalesced run to appear as one operation, not N.
TEST(MemoryPageDeviceTest, RunTransferIsOneTracedOperation) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    device->ClearTrace();
    device->ResetStats();

    std::vector<std::byte> batch(4 * kPageSize);
    ASSERT_TRUE(device->WritePageRun(2, 4, batch).ok());

    ASSERT_EQ(device->trace().size(), 1u);
    EXPECT_EQ(device->trace()[0].kind, MemoryPageDevice::OpKind::kWrite);
    EXPECT_EQ(device->trace()[0].first_page_id, 2u);
    EXPECT_EQ(device->trace()[0].nr_pages, 4u);
    EXPECT_EQ(device->stats().writes, 1u);
    EXPECT_EQ(device->stats().pages_written, 4u);
}

TEST(MemoryPageDeviceTest, TraceRecordsOperationOrder) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_NE(device, nullptr);

    Page page{};
    ASSERT_TRUE(device->EnsureCapacity(1).ok());
    ASSERT_TRUE(device->WritePage(0, Const(page)).ok());
    ASSERT_TRUE(device->ReadPage(0, Mut(page)).ok());
    ASSERT_TRUE(device->Sync().ok());

    ASSERT_EQ(device->trace().size(), 4u);
    EXPECT_EQ(device->trace()[0].kind, MemoryPageDevice::OpKind::kGrow);
    EXPECT_EQ(device->trace()[0].nr_pages, 8u);  // capacity after the grow
    EXPECT_EQ(device->trace()[1].kind, MemoryPageDevice::OpKind::kWrite);
    EXPECT_EQ(device->trace()[2].kind, MemoryPageDevice::OpKind::kRead);
    EXPECT_EQ(device->trace()[3].kind, MemoryPageDevice::OpKind::kSync);
}

}  // namespace
}  // namespace kds::storage
