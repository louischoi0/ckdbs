#include "kds/storage/file_page_device.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

// Fills a page with a pattern derived from `seed` so a mis-addressed read
// is visibly the *wrong page*, not just wrong bytes.
Page PatternPage(std::uint8_t seed) {
    Page page{};
    for (std::size_t i = 0; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
    return page;
}

class FilePageDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = (std::filesystem::temp_directory_path() /
                 ("kds_file_page_device_" + std::string(info->name()) + "_" +
                  std::to_string(::getpid()) + ".dat"))
                    .string();
        std::filesystem::remove(path_);
    }

    void TearDown() override { std::filesystem::remove(path_); }

    std::unique_ptr<FilePageDevice> OpenDevice(std::uint32_t extent_pages = kDefaultExtentPages) {
        auto opened = FilePageDevice::Open(path_, extent_pages);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        return opened.ok() ? std::move(opened.value()) : nullptr;
    }

    std::uint64_t FileSize() const { return std::filesystem::file_size(path_); }

    std::string path_;
};

TEST_F(FilePageDeviceTest, OpenCreatesEmptyFile) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->page_capacity(), 0u);
    EXPECT_TRUE(std::filesystem::exists(path_));
    EXPECT_EQ(FileSize(), 0u);
}

TEST_F(FilePageDeviceTest, OpenRejectsZeroExtent) {
    auto opened = FilePageDevice::Open(path_, 0);
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(FilePageDeviceTest, EnsureCapacityRoundsUpToWholeExtents) {
    auto device = OpenDevice(/*extent_pages=*/8);
    ASSERT_NE(device, nullptr);

    ASSERT_TRUE(device->EnsureCapacity(1).ok());
    EXPECT_EQ(device->page_capacity(), 8u);
    EXPECT_EQ(FileSize(), 8u * kPageSize);

    ASSERT_TRUE(device->EnsureCapacity(9).ok());
    EXPECT_EQ(device->page_capacity(), 16u);
    EXPECT_EQ(FileSize(), 16u * kPageSize);
}

TEST_F(FilePageDeviceTest, EnsureCapacityIsIdempotentAndNeverShrinks) {
    auto device = OpenDevice(/*extent_pages=*/8);
    ASSERT_NE(device, nullptr);

    ASSERT_TRUE(device->EnsureCapacity(16).ok());
    ASSERT_TRUE(device->EnsureCapacity(16).ok());
    EXPECT_EQ(device->page_capacity(), 16u);

    ASSERT_TRUE(device->EnsureCapacity(1).ok());
    EXPECT_EQ(device->page_capacity(), 16u);
    EXPECT_EQ(FileSize(), 16u * kPageSize);
}

TEST_F(FilePageDeviceTest, EnsureCapacityRejectsBeyondDesignCeiling) {
    auto device = OpenDevice();
    ASSERT_NE(device, nullptr);

    const Status status = device->EnsureCapacity(kMaxPageCount + 1);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(device->page_capacity(), 0u);
}

TEST_F(FilePageDeviceTest, WriteThenReadRoundTrips) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(4).ok());

    const Page written = PatternPage(3);
    ASSERT_TRUE(device->WritePage(2, Const(written)).ok());

    Page read{};
    ASSERT_TRUE(device->ReadPage(2, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), written.data(), kPageSize), 0);
}

// Decision S5: the mapping is pure arithmetic, file_offset = page_id *
// 8192. Asserted against the raw file so nothing can quietly introduce an
// indirection layer.
TEST_F(FilePageDeviceTest, PageIdMapsToArithmeticFileOffset) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(8).ok());

    for (PageId id : {0u, 1u, 5u, 7u}) {
        const Page page = PatternPage(static_cast<std::uint8_t>(id + 1));
        ASSERT_TRUE(device->WritePage(id, Const(page)).ok()) << "page " << id;
    }
    ASSERT_TRUE(device->Sync().ok());

    std::ifstream file(path_, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    for (PageId id : {0u, 1u, 5u, 7u}) {
        const Page expected = PatternPage(static_cast<std::uint8_t>(id + 1));
        std::vector<char> raw(kPageSize);
        file.seekg(static_cast<std::streamoff>(id) * kPageSize);
        ASSERT_TRUE(file.read(raw.data(), kPageSize)) << "page " << id;
        EXPECT_EQ(std::memcmp(raw.data(), expected.data(), kPageSize), 0) << "page " << id;
    }
}

TEST_F(FilePageDeviceTest, UnwrittenPageInsideCapacityReadsZeroes) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(4).ok());

    Page read{};
    std::memset(read.data(), 0xFF, read.size());
    ASSERT_TRUE(device->ReadPage(3, Mut(read)).ok());

    for (std::size_t i = 0; i < kPageSize; ++i) {
        ASSERT_EQ(read[i], std::byte{0}) << "byte " << i;
    }
    // And it is therefore an unformatted page, not a valid one.
    EXPECT_FALSE(ValidatePageHeader(Const(read)).ok());
}

TEST_F(FilePageDeviceTest, AccessBeyondCapacityIsOutOfRange) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(4).ok());

    Page page{};
    EXPECT_EQ(device->ReadPage(4, Mut(page)).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->WritePage(4, Const(page)).code(), StatusCode::kOutOfRange);
    EXPECT_EQ(device->ReadPage(kMaxPageCount - 1, Mut(page)).code(), StatusCode::kOutOfRange);
}

TEST_F(FilePageDeviceTest, RunTransfersCoverConsecutivePages) {
    auto device = OpenDevice(/*extent_pages=*/8);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(8).ok());

    constexpr std::uint32_t kRun = 3;
    constexpr PageId kFirst = 2;
    std::vector<std::byte> batch(kRun * kPageSize);
    for (std::uint32_t i = 0; i < kRun; ++i) {
        const Page page = PatternPage(static_cast<std::uint8_t>(i + 11));
        std::memcpy(batch.data() + i * kPageSize, page.data(), kPageSize);
    }
    ASSERT_TRUE(device->WritePageRun(kFirst, kRun, batch).ok());

    // Read the run back one page at a time: the run write must land on
    // exactly the pages a single-page write would have.
    for (std::uint32_t i = 0; i < kRun; ++i) {
        Page read{};
        ASSERT_TRUE(device->ReadPage(kFirst + i, Mut(read)).ok());
        EXPECT_EQ(std::memcmp(read.data(), batch.data() + i * kPageSize, kPageSize), 0)
            << "page " << (kFirst + i);
    }

    std::vector<std::byte> read_back(kRun * kPageSize);
    ASSERT_TRUE(device->ReadPageRun(kFirst, kRun, read_back).ok());
    EXPECT_EQ(std::memcmp(read_back.data(), batch.data(), batch.size()), 0);
}

TEST_F(FilePageDeviceTest, RunRejectsMismatchedBufferAndZeroLength) {
    auto device = OpenDevice(/*extent_pages=*/8);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(8).ok());

    std::vector<std::byte> too_small(2 * kPageSize - 1);
    EXPECT_EQ(device->ReadPageRun(0, 2, too_small).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(device->WritePageRun(0, 2, too_small).code(), StatusCode::kInvalidArgument);

    std::vector<std::byte> empty;
    EXPECT_EQ(device->ReadPageRun(0, 0, empty).code(), StatusCode::kInvalidArgument);
}

TEST_F(FilePageDeviceTest, RunPastCapacityIsOutOfRange) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(4).ok());

    std::vector<std::byte> batch(3 * kPageSize);
    EXPECT_EQ(device->WritePageRun(2, 3, batch).code(), StatusCode::kOutOfRange);
}

TEST_F(FilePageDeviceTest, DataAndCapacitySurviveReopen) {
    const Page written = PatternPage(9);
    {
        auto device = OpenDevice(/*extent_pages=*/8);
        ASSERT_NE(device, nullptr);
        ASSERT_TRUE(device->EnsureCapacity(5).ok());
        ASSERT_TRUE(device->WritePage(4, Const(written)).ok());
        ASSERT_TRUE(device->Sync().ok());
    }

    auto device = OpenDevice(/*extent_pages=*/8);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->page_capacity(), 8u);

    Page read{};
    ASSERT_TRUE(device->ReadPage(4, Mut(read)).ok());
    EXPECT_EQ(std::memcmp(read.data(), written.data(), kPageSize), 0);
}

TEST_F(FilePageDeviceTest, OpenRejectsPartialPageFile) {
    {
        std::ofstream file(path_, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        const std::vector<char> junk(kPageSize + 17, 'x');
        file.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    auto opened = FilePageDevice::Open(path_);
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

// End-to-end with the header codec: a page stamped before write verifies
// after a round trip through the file, and a byte flipped on disk is
// caught on load. This is the checksum contract of page.md section 10 as
// the buffer pool will use it.
TEST_F(FilePageDeviceTest, ChecksummedPageSurvivesRoundTripAndDetectsDiskCorruption) {
    auto device = OpenDevice(/*extent_pages=*/4);
    ASSERT_NE(device, nullptr);
    ASSERT_TRUE(device->EnsureCapacity(4).ok());

    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    std::memset(page.data() + kPageBodyOffset, 0x77, 512);
    StampPageChecksum(Mut(page));
    ASSERT_TRUE(device->WritePage(1, Const(page)).ok());

    Page loaded{};
    ASSERT_TRUE(device->ReadPage(1, Mut(loaded)).ok());
    EXPECT_TRUE(VerifyPageChecksum(Const(loaded)).ok());
    EXPECT_TRUE(ValidatePageHeader(Const(loaded), PageType::kHeap).ok());

    Page corrupted = page;
    corrupted[kPageBodyOffset + 10] ^= std::byte{0x08};
    ASSERT_TRUE(device->WritePage(1, Const(corrupted)).ok());

    ASSERT_TRUE(device->ReadPage(1, Mut(loaded)).ok());
    EXPECT_EQ(VerifyPageChecksum(Const(loaded)).code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::storage
