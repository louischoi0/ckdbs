#include "kds/storage/device_page_store.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/file_page_device.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::unique_ptr<MemoryPageDevice> MakeDevice(std::uint32_t extent_pages = 8,
                                             std::uint32_t initial_pages = 0) {
    auto created = MemoryPageDevice::Create(extent_pages, initial_pages);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.ok() ? std::move(created.value()) : nullptr;
}

std::unique_ptr<DevicePageStore> OpenStore(PageDevice& device, PageId first_new_page_id = 128) {
    auto opened = DevicePageStore::Open(device, first_new_page_id);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// Writes a recognizable pattern into a page handed out by the store, so a
// later read proves it came back from the right page. The pattern goes in
// the body only: bytes 0..kPageBodyOffset are the common page header, and
// the store stamps a checksum there on every write (page.md section 8).
void Fill(std::span<std::byte, kPageSize> page, std::uint8_t seed) {
    FormatPage(page, PageType::kHeap);
    for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
}

bool Matches(std::span<const std::byte, kPageSize> page, std::uint8_t seed) {
    for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
        if (page[i] != static_cast<std::byte>((i + seed * 7u) & 0xFF)) return false;
    }
    return true;
}

TEST(DevicePageStoreTest, FreshDeviceHasOnlyTheFreeMapAllocated) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->allocated_pages(), 1u);
    EXPECT_TRUE(store->IsAllocated(kFreeMapPageId));
    EXPECT_FALSE(store->IsAllocated(0));
    EXPECT_EQ(store->Get(0).status().code(), StatusCode::kNotFound);
    EXPECT_EQ(store->Get(500).status().code(), StatusCode::kNotFound);
}

TEST(DevicePageStoreTest, CreateAtThenGetReturnsTheSamePage) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto created = store->CreateAt(0);
    ASSERT_TRUE(created.ok()) << created.status().message();
    Fill(created.value(), 3);

    auto fetched = store->Get(0);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    EXPECT_EQ(fetched.value().data(), created.value().data());
    EXPECT_TRUE(Matches(fetched.value(), 3));

    EXPECT_EQ(store->CreateAt(0).status().code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(store->CreateAt(kFreeMapPageId).status().code(), StatusCode::kAlreadyExists);
}

TEST(DevicePageStoreTest, CreateNewStartsAtTheConfiguredIdAndAdvances) {
    auto device = MakeDevice();
    auto store = OpenStore(*device, /*first_new_page_id=*/128);
    ASSERT_NE(store, nullptr);

    auto first = store->CreateNew();
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(first.value().first, 128u);

    auto second = store->CreateNew();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().first, 129u);

    // A fixed-id page below the CreateNew watermark does not disturb it.
    ASSERT_TRUE(store->CreateAt(4).ok());
    auto third = store->CreateNew();
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(third.value().first, 130u);
}

TEST(DevicePageStoreTest, PageIdBeyondFreeMapCoverageIsOutOfRange) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->CreateAt(kFreeMapBitsPerPage).status().code(), StatusCode::kOutOfRange);
    EXPECT_FALSE(store->IsAllocated(kFreeMapBitsPerPage));
    EXPECT_EQ(store->Get(kFreeMapBitsPerPage).status().code(), StatusCode::kNotFound);
}

// The point of the whole class: state written before a Sync() is there
// after reopening the same device, and pages that were never created are
// still NotFound rather than zero pages.
TEST(DevicePageStoreTest, SyncedStateSurvivesReopen) {
    auto device = MakeDevice();
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);

        auto zero = store->CreateAt(0);
        ASSERT_TRUE(zero.ok());
        Fill(zero.value(), 1);

        auto user = store->CreateNew();
        ASSERT_TRUE(user.ok());
        Fill(user.value().second, 2);

        ASSERT_TRUE(store->Sync().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->allocated_pages(), 3u);
    EXPECT_EQ(store->resident_pages(), 0u);  // nothing loaded until asked for

    auto zero = store->Get(0);
    ASSERT_TRUE(zero.ok()) << zero.status().message();
    EXPECT_TRUE(Matches(zero.value(), 1));

    auto user = store->Get(128);
    ASSERT_TRUE(user.ok()) << user.status().message();
    EXPECT_TRUE(Matches(user.value(), 2));

    EXPECT_EQ(store->resident_pages(), 2u);
    EXPECT_EQ(store->Get(129).status().code(), StatusCode::kNotFound);

    // The reopened store keeps minting above what the previous one used.
    auto next = store->CreateNew();
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().first, 129u);
}

// Without a WAL this store is restart-durable, not crash-durable
// (docs/wal.md is the missing piece). Pin that boundary down so nobody
// mistakes the Flush ordering for a crash guarantee.
TEST(DevicePageStoreTest, UnsyncedWorkIsLostOnCrash) {
    auto device = MakeDevice();
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        auto created = store->CreateAt(0);
        ASSERT_TRUE(created.ok());
        Fill(created.value(), 4);
    }
    device->Crash();

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->allocated_pages(), 1u);
    EXPECT_EQ(store->Get(0).status().code(), StatusCode::kNotFound);
}

// Flush writes data pages in page-id order (file order, page.md section
// 13) and the free map last, so a crash mid-flush can only orphan a page,
// never publish one whose bytes never landed.
TEST(DevicePageStoreTest, FlushWritesIdSortedWithTheFreeMapLast) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    for (const PageId page_id : {PageId{200}, PageId{7}, PageId{0}, PageId{64}}) {
        ASSERT_TRUE(store->CreateAt(page_id).ok());
    }

    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());

    std::vector<PageId> written;
    for (const auto& entry : device->trace()) {
        if (entry.kind == MemoryPageDevice::OpKind::kWrite) written.push_back(entry.first_page_id);
    }
    ASSERT_EQ(written.size(), 5u);
    EXPECT_EQ(written.back(), kFreeMapPageId);

    std::vector<PageId> data(written.begin(), written.end() - 1);
    EXPECT_EQ(data, (std::vector<PageId>{0, 7, 64, 200}));

    // A second flush with nothing dirtied is a no-op.
    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());
    EXPECT_TRUE(device->trace().empty());
}

TEST(DevicePageStoreTest, OpenRejectsACorruptedFreeMap) {
    auto device = MakeDevice();
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->CreateAt(0).ok());
        ASSERT_TRUE(store->Sync().ok());
    }

    Page free_map{};
    ASSERT_TRUE(device->ReadPage(kFreeMapPageId, std::span<std::byte, kPageSize>(free_map)).ok());
    free_map[kPageBodyOffset + 3] ^= std::byte{0x08};
    ASSERT_TRUE(
        device->WritePage(kFreeMapPageId, std::span<const std::byte, kPageSize>(free_map)).ok());

    auto opened = DevicePageStore::Open(*device);
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

// A free map claiming a page the device cannot address is a disagreement
// between the two, not a page to read: it must be reported, not papered
// over with zeroes.
TEST(DevicePageStoreTest, AllocatedPageBeyondDeviceCapacityIsCorruption) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/8);

    Page free_map{};
    auto view = std::span<std::byte, kPageSize>(free_map);
    FormatFreeMapPage(view);
    FreeMapAllocate(view, kFreeMapPageId);
    FreeMapAllocate(view, 1000);  // well past the device's 8 pages
    StampPageChecksum(view);
    ASSERT_TRUE(device->WritePage(kFreeMapPageId, std::span<const std::byte, kPageSize>(free_map))
                    .ok());

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->IsAllocated(1000));
    EXPECT_EQ(store->Get(1000).status().code(), StatusCode::kCorruption);
}

TEST(DevicePageStoreTest, GrowsTheDeviceToCoverNewPages) {
    auto device = MakeDevice(/*extent_pages=*/8);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(device->page_capacity(), 8u);

    ASSERT_TRUE(store->CreateAt(300).ok());
    EXPECT_GE(device->page_capacity(), 301u);
    ASSERT_TRUE(store->Sync().ok());

    auto reopened = OpenStore(*device);
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(reopened->Get(300).ok());
}

// The same round trip through a real file, which is what the server runs.
TEST(DevicePageStoreTest, StateSurvivesReopenOnAFile) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("kds_device_page_store_" + std::to_string(::getpid()) + ".dat"))
            .string();
    std::filesystem::remove(path);

    {
        auto device = FilePageDevice::Open(path);
        ASSERT_TRUE(device.ok()) << device.status().message();
        auto store = OpenStore(*device.value());
        ASSERT_NE(store, nullptr);

        auto created = store->CreateAt(0);
        ASSERT_TRUE(created.ok());
        Fill(created.value(), 11);
        ASSERT_TRUE(store->Sync().ok());
    }

    auto device = FilePageDevice::Open(path);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = OpenStore(*device.value());
    ASSERT_NE(store, nullptr);

    auto fetched = store->Get(0);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    EXPECT_TRUE(Matches(fetched.value(), 11));

    std::filesystem::remove(path);
}

// Every page written through the store carries a valid checksum, and a bit
// flipped underneath it is caught on the next load rather than served.
TEST(DevicePageStoreTest, ChecksumsAreStampedOnWriteAndVerifiedOnLoad) {
    auto device = MakeDevice();
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        auto page = store->CreateAt(0);
        ASSERT_TRUE(page.ok());
        Fill(page.value(), 21);
        ASSERT_TRUE(store->Sync().ok());
    }

    Page raw{};
    ASSERT_TRUE(device->ReadPage(0, std::span<std::byte, kPageSize>(raw)).ok());
    EXPECT_TRUE(VerifyPageChecksum(std::span<const std::byte, kPageSize>(raw)).ok());

    raw[kPageBodyOffset + 500] ^= std::byte{0x40};
    ASSERT_TRUE(device->WritePage(0, std::span<const std::byte, kPageSize>(raw)).ok());

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->Get(0).status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::storage
