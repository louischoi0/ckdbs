#include "kds/storage/device_page_store.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/extent_lease.hpp"
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

TEST(DevicePageStoreTest, FreshDeviceHasOnlyTheTwoMapsAllocated) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // The allocation bitmap and the headerless bitmap, both reserved at
    // fixed ids and both self-allocated at open.
    EXPECT_EQ(store->allocated_pages(), 2u);
    EXPECT_TRUE(store->IsAllocated(kFreeMapPageId));
    EXPECT_TRUE(store->IsAllocated(kHeaderlessMapPageId));
    // The maps themselves are headered, so they are checksummed like
    // anything else - only what they *point at* can be headerless.
    EXPECT_FALSE(store->IsHeaderless(kHeaderlessMapPageId));
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
    Fill(created.value().bytes(), 3);

    auto fetched = store->Get(0);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    EXPECT_EQ(fetched.value().bytes().data(), created.value().bytes().data());
    EXPECT_TRUE(Matches(fetched.value().bytes(), 3));

    EXPECT_EQ(store->CreateAt(0).status().code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(store->CreateAt(kFreeMapPageId).status().code(), StatusCode::kAlreadyExists);
}

TEST(DevicePageStoreTest, StampPageLsnStampsTheOwningStream) {
    // PW1c-3 (spec-page-lsn-cross-stream.md §9 rule 4): every logged
    // mutation funnels through StampPageLsn, so the stream stamp rides the
    // LSN stamp - core_id + 1, and this store's default identity is core 0.
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto created = store->CreateAt(0);
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(GetPageStreamStamp(created.value().bytes()), 0u) << "unstamped until logged";

    ASSERT_TRUE(store->StampPageLsn(0, /*lsn=*/64).ok());
    EXPECT_EQ(GetPageStreamStamp(created.value().bytes()), 1u);
    EXPECT_EQ(GetPageLsn(created.value().bytes()), 64u);
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
        Fill(zero.value().bytes(), 1);

        auto user = store->CreateNew();
        ASSERT_TRUE(user.ok());
        Fill(user.value().second.bytes(), 2);

        ASSERT_TRUE(store->Sync().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->allocated_pages(), 4u);  // two data pages + the two maps
    EXPECT_EQ(store->resident_pages(), 0u);  // nothing loaded until asked for

    auto zero = store->Get(0);
    ASSERT_TRUE(zero.ok()) << zero.status().message();
    EXPECT_TRUE(Matches(zero.value().bytes(), 1));

    auto user = store->Get(128);
    ASSERT_TRUE(user.ok()) << user.status().message();
    EXPECT_TRUE(Matches(user.value().bytes(), 2));

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
        Fill(created.value().bytes(), 4);
    }
    device->Crash();

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->allocated_pages(), 2u);  // the two maps, nothing else
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
    // Four data pages, then both maps. The free map is strictly last: it
    // is what makes an id exist, so a crash before it can only orphan a
    // page, never publish one whose bytes never landed.
    ASSERT_EQ(written.size(), 6u);
    EXPECT_EQ(written.back(), kFreeMapPageId);
    EXPECT_EQ(written[written.size() - 2], kHeaderlessMapPageId);

    std::vector<PageId> data(written.begin(), written.end() - 2);
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

// A free map claiming a page the device cannot address is not a page to
// read - and since PW1c-7 it is NotFound rather than Corruption: an extent
// reserved for a peer is allocated whole in the map core 0 flushes while
// the peer writes its pages lazily, so "allocated, never written" is an
// ordinary state, and the code redo needs for it is the one its PAGE_INIT
// arm creates from (wal/redo.cpp). Nothing is papered over with zeroes: the
// read still fails, and only a logged PAGE_INIT may create the page.
TEST(DevicePageStoreTest, AllocatedPageBeyondDeviceCapacityIsNeverWritten) {
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
    EXPECT_EQ(store->Get(1000).status().code(), StatusCode::kNotFound);
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
        Fill(created.value().bytes(), 11);
        ASSERT_TRUE(store->Sync().ok());
    }

    auto device = FilePageDevice::Open(path);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = OpenStore(*device.value());
    ASSERT_NE(store, nullptr);

    auto fetched = store->Get(0);
    ASSERT_TRUE(fetched.ok()) << fetched.status().message();
    EXPECT_TRUE(Matches(fetched.value().bytes(), 11));

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
        Fill(page.value().bytes(), 21);
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


// ---- Headerless pages ---------------------------------------------------
//
// A headerless page's payload tiles 8 KiB exactly and carries no common
// header (docs/page.md section 1), so byte 4 -
// where every other page keeps its checksum - is data. These tests are
// about the two moments that would destroy it: the stamp on write-out and
// the verify on read-back.

// Every byte distinct from its neighbours *including the header region*,
// which is what a headerless page actually looks like.
void FillWhole(std::span<std::byte, kPageSize> page, std::uint8_t seed) {
    for (std::size_t i = 0; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i * 31u + seed) & 0xFF);
    }
}

bool MatchesWhole(std::span<const std::byte, kPageSize> page, std::uint8_t seed) {
    for (std::size_t i = 0; i < kPageSize; ++i) {
        if (page[i] != static_cast<std::byte>((i * 31u + seed) & 0xFF)) return false;
    }
    return true;
}

TEST(DevicePageStoreHeaderlessTest, AHeaderlessPageIsMarkedAndAHeaderedOneIsNot) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto plain = store->CreateNew();
    ASSERT_TRUE(plain.ok());
    auto raw = store->CreateNewHeaderless();
    ASSERT_TRUE(raw.ok());

    EXPECT_FALSE(store->IsHeaderless(plain.value().first));
    EXPECT_TRUE(store->IsHeaderless(raw.value().first));
    // An id nothing allocated is treated as headered - the safe default,
    // since it means "verify" rather than "trust".
    EXPECT_FALSE(store->IsHeaderless(50000));
}

TEST(DevicePageStoreHeaderlessTest, FlushDoesNotStampAChecksumOverItsBytes) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto raw = store->CreateNewHeaderless();
    ASSERT_TRUE(raw.ok());
    const PageId id = raw.value().first;
    FillWhole(raw.value().second.bytes(), 3);

    ASSERT_TRUE(store->Flush().ok());

    // Still byte-identical in the frame: the stamp would have overwritten
    // bytes 4..8, which on a headerless page is live entry data.
    auto after = store->Get(id);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(MatchesWhole(after.value().bytes(), 3));
}

TEST(DevicePageStoreHeaderlessTest, ItSurvivesAReopenWithoutBeingCalledCorrupt) {
    // The reason the headerless map has to be durable at all. This store
    // never evicts, so a page comes off the device exactly once - here -
    // and an in-memory-only set would have been lost by now.
    auto device = MakeDevice();
    PageId id = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        auto raw = store->CreateNewHeaderless();
        ASSERT_TRUE(raw.ok());
        id = raw.value().first;
        FillWhole(raw.value().second.bytes(), 9);
        ASSERT_TRUE(store->Sync().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->IsHeaderless(id)) << "the mark must be durable, not a side table";

    auto page = store->Get(id);
    ASSERT_TRUE(page.ok()) << "a headerless page must not be checksum-verified: "
                           << page.status().message();
    EXPECT_TRUE(MatchesWhole(page.value().bytes(), 9));
}

TEST(DevicePageStoreHeaderlessTest, HeaderedPagesAreStillStampedAndVerified) {
    // The change must not have turned verification off for everything.
    auto device = MakeDevice();
    PageId id = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        auto plain = store->CreateNew();
        ASSERT_TRUE(plain.ok());
        id = plain.value().first;
        Fill(plain.value().second.bytes(), 4);
        ASSERT_TRUE(store->Sync().ok());
    }

    // Corrupt one body byte behind the store's back.
    Page bytes{};
    std::span<std::byte, kPageSize> view(bytes);
    ASSERT_TRUE(device->ReadPage(id, view).ok());
    bytes[kPageBodyOffset + 10] ^= std::byte{0xFF};
    ASSERT_TRUE(device->WritePage(id, std::span<const std::byte, kPageSize>(bytes)).ok());

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->Get(id).status().code(), StatusCode::kCorruption);
}

TEST(DevicePageStoreHeaderlessTest, DamageToAHeaderlessPageIsSilentByDesign) {
    // Stated as a test because it is a deliberate trade, not an oversight:
    // these pages carry no checksum, so bit-rot in one is undetectable
    // here. It is survivable instead - the probe's Keystone-id check
    // (spec section 3.1) turns a wrong entry into a miss and a fallback
    // scan, which is a stronger guarantee than detection.
    auto device = MakeDevice();
    PageId id = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        auto raw = store->CreateNewHeaderless();
        ASSERT_TRUE(raw.ok());
        id = raw.value().first;
        FillWhole(raw.value().second.bytes(), 2);
        ASSERT_TRUE(store->Sync().ok());
    }

    Page bytes{};
    std::span<std::byte, kPageSize> view(bytes);
    ASSERT_TRUE(device->ReadPage(id, view).ok());
    bytes[100] ^= std::byte{0xFF};
    ASSERT_TRUE(device->WritePage(id, std::span<const std::byte, kPageSize>(bytes)).ok());

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    auto page = store->Get(id);
    EXPECT_TRUE(page.ok()) << "no checksum means no detection, by construction";
    EXPECT_FALSE(MatchesWhole(page.value().bytes(), 2));
}

TEST(DevicePageStoreHeaderlessTest, TheMarkIsWrittenBeforeTheFreeMapPublishesTheId) {
    // Ordering that matters on a crash: the free map is what makes an id
    // exist, so it goes last. The reverse would publish an allocated
    // headerless page whose bit had not landed, and the next read
    // of it would verify a checksum nobody wrote.
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto raw = store->CreateNewHeaderless();
    ASSERT_TRUE(raw.ok());
    FillWhole(raw.value().second.bytes(), 1);

    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());

    std::vector<PageId> written;
    for (const auto& entry : device->trace()) {
        if (entry.kind == MemoryPageDevice::OpKind::kWrite) written.push_back(entry.first_page_id);
    }
    ASSERT_GE(written.size(), 2u);
    EXPECT_EQ(written.back(), kFreeMapPageId);
    EXPECT_EQ(written[written.size() - 2], kHeaderlessMapPageId);
}

// ---- Core ownership and leases (workplan-crosscore.md M5, P2) ---------
//
// A store bound to a non-system core allocates from a lease and **never
// touches the free map**, which is what keeps that one durable page
// single-owner. These pin both halves: that it uses the lease, and that it
// leaves the map alone.

TEST(DevicePageStoreOwnershipTest, ALeasedStoreAllocatesOnlyFromItsExtent) {
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    LeasedIdSource lease(Extent{1000, 3});
    store->SetCoreOwnership(/*core_id=*/2, &lease);

    for (PageId expected : {1000u, 1001u, 1002u}) {
        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        EXPECT_EQ(created.value().first, expected);
    }

    // Spent, and the failure is retryable rather than "the disk is full".
    auto spent = store->CreateNew();
    ASSERT_FALSE(spent.ok());
    EXPECT_EQ(spent.status().code(), StatusCode::kResourceExhausted);
}

TEST(DevicePageStoreOwnershipTest, AWriteGrantAdmitsExactPagesAndNothingElse) {
    // PW1c-4 (workplan-peer-writer.md §8 rule 1): write rights are
    // exact-page, never extent - a fault grant's superset stays unwritable,
    // which is the objection that ruled out widening GrantFaultPages.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    LeasedIdSource lease(Extent{1000, 4});
    store->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/128);

    // Own lease writable; a core-0 page and the system range are not.
    EXPECT_TRUE(store->MayWrite(1000));
    EXPECT_FALSE(store->MayWrite(130));
    EXPECT_FALSE(store->MayWrite(4));

    const PageId granted[] = {130, 131};
    store->GrantWritePages(granted);
    EXPECT_TRUE(store->MayWrite(130));
    EXPECT_TRUE(store->MayWrite(131));
    EXPECT_FALSE(store->MayWrite(132)) << "the rest of the extent stays unwritable";
    EXPECT_FALSE(store->MayWrite(4)) << "a grant never reaches the system range";

    // Idempotent re-grant (a republish resends), and order-independent.
    const PageId regrant[] = {131, 130};
    store->GrantWritePages(regrant);
    EXPECT_TRUE(store->MayWrite(130));
    EXPECT_TRUE(store->MayWrite(131));
}

TEST(DevicePageStoreOwnershipTest, ALeasedStoreNeverMutatesTheFreeMap) {
    // The guideline-1 property: the free map is core 0's, so a second writer
    // would be shared mutable state between cores.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    const std::uint32_t before = store->allocated_pages();

    LeasedIdSource lease(Extent{1000, 4});
    store->SetCoreOwnership(2, &lease);
    ASSERT_TRUE(store->CreateNew().ok());
    ASSERT_TRUE(store->CreateNew().ok());

    EXPECT_EQ(store->allocated_pages(), before)
        << "a leased core set bits in the free map it does not own";
}

TEST(DevicePageStoreOwnershipTest, ALeasedStoreNeverWritesTheMapsBackToTheDevice) {
    // The write-out half of the rule above, and the one a peer reaches: the
    // map bit a leased store can hold is redo's, set by `CreateAt` at mount
    // *before* the lease is installed (server/core_runtime.cpp orders it so),
    // and `FlushMaps` is the only path to those two page ids that does not go
    // through MayWrite. A peer that published its copy would write the map as
    // it stood when this store opened - reverting every allocation core 0 has
    // made since, which is silent reuse of live pages.
    auto device = MakeDevice(64, 0);

    auto core0 = OpenStore(*device);
    ASSERT_NE(core0, nullptr);
    ASSERT_TRUE(core0->CreateNew().ok());
    ASSERT_TRUE(core0->Sync().ok());

    // The peer's copy of the map: taken here, and stale from the next line on.
    auto peer = OpenStore(*device);
    ASSERT_NE(peer, nullptr);

    auto later = core0->CreateNew();
    ASSERT_TRUE(later.ok()) << later.status().message();
    const PageId core0_page = later.value().first;
    ASSERT_TRUE(core0->Sync().ok());

    // What redo does on a peer's stream, at the point the lease does not
    // exist yet: a page placed at a chosen id, which marks the map.
    ASSERT_TRUE(peer->CreateAt(300).ok());
    LeasedIdSource lease(Extent{1000, 4});
    peer->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/128);

    ASSERT_TRUE(peer->Sync().ok());

    Page map{};
    ASSERT_TRUE(device->ReadPage(kFreeMapPageId, std::span<std::byte, kPageSize>(map)).ok());
    EXPECT_TRUE(FreeMapIsAllocated(std::span<const std::byte, kPageSize>(map), core0_page))
        << "a leased store wrote its stale free map over core 0's";
    EXPECT_FALSE(FreeMapIsAllocated(std::span<const std::byte, kPageSize>(map), 300u))
        << "a leased store published a free-map bit it does not own";
}

TEST(DevicePageStoreOwnershipTest, ALeasedPageIsReadableThoughTheMapDoesNotKnowIt) {
    // A non-zero core reads its free map at Open(); core 0 marks the lease's
    // bits later, in *its* copy. So the lease has to answer for the core's
    // own ids or every page it allocates reads back NotFound.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    LeasedIdSource lease(Extent{1000, 2});
    store->SetCoreOwnership(2, &lease);

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok());
    const PageId id = created.value().first;
    EXPECT_TRUE(store->IsAllocated(id));

    auto again = store->Get(id);
    EXPECT_TRUE(again.ok()) << again.status().message();
}

TEST(DevicePageStoreOwnershipTest, ALeasedStoreMayNotPlaceAPageAtAChosenId) {
    // CreateAt is a claim on the free map. Every caller of it is bootstrap
    // or a fixed system page, all core 0's - so this is unreachable rather
    // than restrictive, and the check is here so it stays that way.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    LeasedIdSource lease(Extent{1000, 4});
    store->SetCoreOwnership(2, &lease);

    EXPECT_EQ(store->CreateAt(300).status().code(), StatusCode::kInvalidArgument);
}

TEST(DevicePageStoreOwnershipTest, TheSystemCoreMayFaultAnything) {
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // No lease installed is core 0's arrangement, and it is also every
    // construction site that predates multicore.
    EXPECT_EQ(store->core_id(), 0u);
    EXPECT_TRUE(store->MayFault(1));
    EXPECT_TRUE(store->MayFault(50'000));
}

TEST(DevicePageStoreOwnershipTest, ALeasedCoreMayNotFaultAForeignPage) {
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // A page that genuinely exists, allocated before the store became a
    // leased one - so this is an ownership refusal and not a NotFound.
    auto other = store->CreateNew();
    ASSERT_TRUE(other.ok());
    const PageId foreign = other.value().first;
    ASSERT_TRUE(store->Sync().ok());

    LeasedIdSource lease(Extent{1000, 4});
    store->SetCoreOwnership(2, &lease);

    EXPECT_FALSE(store->MayFault(foreign));
    EXPECT_TRUE(store->MayFault(1000));

#ifndef NDEBUG
    // Debug builds refuse the fault outright. In release the check is
    // compiled out, so this says nothing there and the test does not ask.
    auto refused = store->Get(foreign);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
#endif
}

TEST(DevicePageStoreOwnershipTest, APageStampedByThisStreamIsClaimedWithoutAGrant) {
    // PW1c-7 (workplan-peer-writer.md §8): every lease and grant is
    // memory-resident, so after a restart a core holds rights over none of
    // the pages it wrote - but each carries the PL-C stamp of the stream
    // that last wrote it (PL §9 rule 4), and rule 6 lets no page leave a
    // stream unrestamped. So a page whose stamp names this core is claimed
    // on the fault, read or write, and nothing else is: a foreign stamp is
    // another core's page and 0 is a page no stream has written since it
    // was formatted (a creation page never acquired - the grant path's
    // job, never a claim's).
    //
    // "A previous run of core 2" is a store over the device that stamps
    // and flushes; "the restart" is a fresh core-2 store whose lease does
    // not cover those pages.
    auto device = MakeDevice(64, 0);
    PageId own = kInvalidPageId, own_read = kInvalidPageId, foreign = kInvalidPageId,
           blank = kInvalidPageId;
    {
        auto previous = OpenStore(*device);
        ASSERT_NE(previous, nullptr);
        auto stamped = [&](std::uint16_t stamp) -> PageId {
            auto created = previous->CreateNew();
            EXPECT_TRUE(created.ok()) << created.status().message();
            if (!created.ok()) return kInvalidPageId;
            Fill(created.value().second.bytes(), 1);
            SetPageStreamStamp(created.value().second.bytes(), stamp);
            return created.value().first;
        };
        own = stamped(StreamStampFor(2));
        own_read = stamped(StreamStampFor(2));
        foreign = stamped(StreamStampFor(0));
        blank = stamped(0);
        ASSERT_TRUE(previous->Sync().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    LeasedIdSource lease(Extent{1000, 4});
    store->SetCoreOwnership(/*core_id=*/2, &lease, /*system_page_limit=*/128);

    // Before the fault: no lease, no grant, no rights - as any restart.
    EXPECT_FALSE(store->MayFault(own));
    EXPECT_FALSE(store->MayWrite(own));
    EXPECT_EQ(store->stamp_claims(), 0u);

    // A write fault claims, and the claim is both rights at once.
    auto written = store->Get(own);
    ASSERT_TRUE(written.ok()) << written.status().message();
    EXPECT_TRUE(Matches(written.value().bytes(), 1)) << "the claim's read is the miss path's";
    EXPECT_TRUE(store->MayWrite(own));
    EXPECT_TRUE(store->MayFault(own));
    EXPECT_EQ(store->stamp_claims(), 1u);

    // A read fault claims too, so the first SELECT after a restart is what
    // makes the next INSERT writable.
    ASSERT_TRUE(store->GetForRead(own_read).ok());
    EXPECT_TRUE(store->MayWrite(own_read));
    EXPECT_EQ(store->stamp_claims(), 2u);

    // Another stream's stamp and no stamp: refused for writes in every
    // build, and the claim count does not move.
    for (PageId page : {foreign, blank}) {
        auto refused = store->Get(page);
        EXPECT_FALSE(refused.ok()) << "page " << page << " must not be writable";
        if (!refused.ok()) EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
        EXPECT_FALSE(store->MayWrite(page));
    }
    EXPECT_EQ(store->stamp_claims(), 2u);
#ifndef NDEBUG
    // And for reads where the fault check is enforced.
    EXPECT_FALSE(store->GetForRead(foreign).ok());
    EXPECT_FALSE(store->GetForRead(blank).ok());
#endif

    // Idempotent: a second fault of a claimed page is a hit, not a claim.
    ASSERT_TRUE(store->GetForRead(own).ok());
    EXPECT_EQ(store->stamp_claims(), 2u);
}

TEST(DevicePageStoreTest, AnAllocatedPageNeverWrittenIsNotFoundNotCorrupt) {
    // Found by PW1c-7's restart test (workplan-peer-writer.md §8): an
    // extent reserved for a peer is allocated whole in the map core 0
    // flushes, while the peer writes its pages lazily - so a crash between
    // a page's PAGE_INIT and its first write-back leaves a page the map
    // calls allocated and the device holds as zeros. Reading it used to be
    // a checksum Corruption, which redo can only poison and wait for a full
    // page image to heal; as NotFound, redo's PAGE_INIT arm creates it, and
    // the peer remounts.
    auto device = MakeDevice(64, /*initial_pages=*/256);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    ExtentAllocator extents(store->free_map_bytes(), /*hint=*/128);
    auto lease = extents.Reserve(8);
    ASSERT_TRUE(lease.ok()) << lease.status().message();
    ASSERT_TRUE(store->Sync().ok());  // the map is durable, the pages are not
    const PageId page = lease.value().first;
    ASSERT_TRUE(store->IsAllocated(page));

    auto got = store->GetForRead(page);
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.status().code(), StatusCode::kNotFound) << got.status().message();
    EXPECT_NE(got.status().message().find("never written"), std::string::npos)
        << got.status().message();

    // What redo does with a NotFound under a PAGE_INIT: the page exists
    // after it, and reads back as what was written.
    auto created = store->CreateAt(page);
    ASSERT_TRUE(created.ok()) << created.status().message();
    Fill(created.value().bytes(), 3);
    ASSERT_TRUE(store->Sync().ok());
    auto again = store->GetForRead(page);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_TRUE(Matches(again.value().bytes(), 3));
}

}  // namespace
}  // namespace kds::storage
