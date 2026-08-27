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

TEST(DevicePageStoreTest, FreshDeviceHasOnlyTheFreeMapAllocated) {
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // The allocation bitmap, self-allocated at open. **Not the headerless
    // bitmap**: since FM6 it is not built until something is headerless,
    // and an id is marked allocated only when its page is placed - an
    // allocated id whose bytes were never written is the signature of a
    // torn creation, which the simulation harness's integrity sweep reads
    // every allocated page to catch.
    EXPECT_EQ(store->allocated_pages(), 1u);
    EXPECT_TRUE(store->IsAllocated(kFreeMapPageId));
    EXPECT_FALSE(store->IsAllocated(kHeaderlessMapPageId));
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
    // PW1c-3 (page-lsn-cross-stream.md §9 rule 4): every logged
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

TEST(DevicePageStoreTest, PageIdBeyondTheDesignCeilingIsOutOfRange) {
    // This pinned kFreeMapBitsPerPage until the free map became multi-page:
    // one bitmap page's coverage *was* the instance ceiling. FM3 moved the
    // refusal to where it belongs, and an id one page past region 0 is now
    // an ordinary page - which the FreeMapRegionTest cases below cover.
    auto device = MakeDevice();
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->CreateAt(kMaxPageCount).status().code(), StatusCode::kOutOfRange);
    EXPECT_FALSE(store->IsAllocated(kMaxPageCount));
    EXPECT_EQ(store->Get(kMaxPageCount).status().code(), StatusCode::kNotFound);
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
    EXPECT_EQ(store->allocated_pages(), 3u);  // two data pages + the free map
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
// (docs/spec/wal.md is the missing piece). Pin that boundary down so nobody
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
    EXPECT_EQ(store->allocated_pages(), 1u);  // the free map, nothing else
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

    const auto writes = [&device]() {
        std::vector<PageId> written;
        for (const auto& entry : device->trace()) {
            if (entry.kind == MemoryPageDevice::OpKind::kWrite) {
                written.push_back(entry.first_page_id);
            }
        }
        return written;
    };

    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());

    // Four data pages, then the free map - and **no headerless map**, which
    // this database has no page for (FM6). The free map is strictly last:
    // it is what makes an id exist, so a crash before it can only orphan a
    // page, never publish one whose bytes never landed.
    std::vector<PageId> written = writes();
    ASSERT_EQ(written.size(), 5u);
    EXPECT_EQ(written.back(), kFreeMapPageId);
    std::vector<PageId> data(written.begin(), written.end() - 1);
    EXPECT_EQ(data, (std::vector<PageId>{0, 7, 64, 200}));

    // A second flush with nothing dirtied is a no-op.
    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());
    EXPECT_TRUE(device->trace().empty());

    // Once a headerless page exists the pair goes out together, and the
    // order within it is the one that matters: headerless first, free map
    // last. The reverse would publish an allocated headerless page whose
    // headerless bit had not landed, and the next read of it would verify
    // a checksum that was never written and call the page corrupt.
    ASSERT_TRUE(store->CreateNewHeaderless().ok());
    device->ClearTrace();
    ASSERT_TRUE(store->Flush().ok());

    written = writes();
    ASSERT_GE(written.size(), 3u);
    EXPECT_EQ(written.back(), kFreeMapPageId);
    EXPECT_EQ(written[written.size() - 2], kHeaderlessMapPageId);
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
// header (docs/spec/page.md section 1), so byte 4 -
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

    // Spent, and the failure is retryable rather than "the disk is full" -
    // kTxnConflict because that is the one code the wire's `retryable` bit
    // follows (status.hpp's IsRetryable).
    auto spent = store->CreateNew();
    ASSERT_FALSE(spent.ok());
    EXPECT_EQ(spent.status().code(), StatusCode::kTxnConflict);
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

TEST(DevicePageStoreOwnershipTest, ARefreshAdoptsTheDevicesBitsAndSubtractsNone) {
    // `RefreshFreeMapFromDevice`'s two halves, and the second is the one
    // no caller can demonstrate: a leased store's own ids answer from the
    // lease (`IsAllocated` short-circuits on `Owns`), so a test that
    // refreshes over a *leased* page proves nothing about the merge. The
    // only bit that lives in this copy and not on the device is redo's -
    // `CreateAt` at mount, before the lease is installed
    // (server/core_runtime.cpp orders it so), which is the construction
    // `ALeasedStoreNeverWritesTheMapsBackToTheDevice` above uses for the
    // write-out half of the same rule. Replacement instead of union would
    // subtract exactly that page.
    auto device = MakeDevice(64, 0);

    auto core0 = OpenStore(*device);
    ASSERT_NE(core0, nullptr);
    ASSERT_TRUE(core0->Sync().ok());

    // The peer's copy: taken here, and stale from core 0's next allocation.
    auto peer = OpenStore(*device);
    ASSERT_NE(peer, nullptr);

    // Redo's bit, in this copy alone - never flushed, and outside the lease
    // below so nothing but the map can answer for it.
    ASSERT_TRUE(peer->CreateAt(300).ok());
    LeasedIdSource lease(Extent{1000, 4});
    peer->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/128);

    auto later = core0->CreateNew();
    ASSERT_TRUE(later.ok()) << later.status().message();
    const PageId core0_page = later.value().first;
    ASSERT_FALSE(peer->IsAllocated(core0_page)) << "the snapshot predates it";
    ASSERT_TRUE(core0->Sync().ok());

    ASSERT_TRUE(peer->RefreshFreeMapFromDevice().ok());

    EXPECT_TRUE(peer->IsAllocated(core0_page)) << "the device's bit was not adopted";
    EXPECT_TRUE(peer->IsAllocated(300u))
        << "the refresh replaced the copy instead of unioning into it, subtracting the page "
           "this store's own recovery rebuilt";
}

TEST(DevicePageStoreOwnershipTest, TheSystemCoresOwnMapIsNotRefreshable) {
    // The refusal is the contract, not a guard: core 0's copy *is* the
    // authority, so re-reading the device over it would overwrite the
    // allocations it has made since its last flush.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->RefreshFreeMapFromDevice().code(), StatusCode::kInvalidArgument);
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

TEST(DevicePageStoreOwnershipTest, APeerAdoptsTheDeviceMapBeforeCallingASystemPageAbsent) {
    // G1 (docs/inflight/known-gaps.md): a peer's free-map copy is a **mount-time
    // snapshot**, advanced only by a relation fault/write grant. Core 0
    // allocating a system page after that - a catalog page, which a peer
    // must read to resolve any relation of its own - left an id the peer's
    // copy has no bit for, and the fault seam turned that staleness into a
    // permanent NotFound: nothing on the failing path asked the device
    // again, so the relation stayed unwritable for the rest of the mount.
    //
    // The shape here is that sequence, minus the 58 index builds it took to
    // reach it at the server: the peer opens first (its snapshot predates
    // the page), core 0 then allocates and flushes.
    auto device = MakeDevice(64, 0);
    auto core0 = OpenStore(*device, /*first_new_page_id=*/3);
    ASSERT_NE(core0, nullptr);
    auto peer = OpenStore(*device, /*first_new_page_id=*/3);
    ASSERT_NE(peer, nullptr);

    LeasedIdSource lease(Extent{1000, 4});
    peer->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/128);

    // Core 0 grows the system range after the peer's snapshot was taken,
    // and publishes both halves - the bytes and the map bit.
    auto grown = core0->CreateNew();
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    const PageId later_system_page = grown.value().first;
    ASSERT_LT(later_system_page, 128u) << "the test needs a page in the peer's readable system range";
    Fill(grown.value().second.bytes(), 3);
    ASSERT_TRUE(core0->Sync().ok());

    EXPECT_FALSE(peer->IsAllocated(later_system_page))
        << "the premise: the peer's copy predates this page";

    auto read = peer->GetForRead(later_system_page);
    ASSERT_TRUE(read.ok()) << read.status().message();
    EXPECT_TRUE(Matches(read.value().bytes(), 3));
    EXPECT_EQ(peer->map_refreshes_on_miss(), 1u)
        << "the adoption is what healed the miss, not an accident of timing";

    // Adopted, so the next read costs nothing: the miss does not repeat.
    auto again = peer->GetForRead(later_system_page);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(peer->map_refreshes_on_miss(), 1u);

    // And an id that genuinely is not allocated anywhere still refuses -
    // naming itself, which is what the bare "page id not found" withheld.
    auto absent = peer->GetForRead(120);
    EXPECT_FALSE(absent.ok());
    EXPECT_EQ(absent.status().code(), StatusCode::kNotFound);
    EXPECT_NE(absent.status().message().find("page id 120"), std::string::npos)
        << absent.status().message();
    EXPECT_EQ(peer->map_refreshes_on_miss(), 2u);
}

TEST(DevicePageStoreOwnershipTest, APeerAdoptsARegionThatDidNotExistAtItsMount) {
    // The same defect one level up, and the FM series is what made it
    // reachable: RefreshFreeMapFromDevice walks *resident* regions, and an
    // absent one reads as all zeroes - so before this, a page core 0
    // placed in a region created after the peer mounted could not be
    // adopted at all, even one the peer had been granted. The refusal was
    // permanent for exactly the same reason.
    auto device = MakeDevice(64, 0);
    auto core0 = OpenStore(*device, /*first_new_page_id=*/3);
    ASSERT_NE(core0, nullptr);
    auto peer = OpenStore(*device, /*first_new_page_id=*/3);
    ASSERT_NE(peer, nullptr);

    LeasedIdSource lease(Extent{1000, 4});
    peer->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/128);

    // Core 0 grows the map into region 1, which the peer has never seen.
    const PageId in_region_one = kFreeMapBitsPerPage + 64;
    ASSERT_TRUE(core0->RaiseAllocationFloor(in_region_one).ok());
    auto grown = core0->CreateNew();
    ASSERT_TRUE(grown.ok()) << grown.status().message();
    ASSERT_EQ(FreeMapRegionOf(grown.value().first), 1u);
    Fill(grown.value().second.bytes(), 5);
    ASSERT_TRUE(core0->Sync().ok());

    // The peer is granted the page: rights alone were never the problem.
    peer->GrantFaultPages(Extent{grown.value().first, 1});
    EXPECT_TRUE(peer->MayFault(grown.value().first));
    EXPECT_FALSE(peer->IsAllocated(grown.value().first))
        << "the premise: region 1 is not resident here";

    auto read = peer->GetForRead(grown.value().first);
    ASSERT_TRUE(read.ok()) << read.status().message();
    EXPECT_TRUE(Matches(read.value().bytes(), 5));
    EXPECT_EQ(peer->map_refreshes_on_miss(), 1u);

    // Loaded once, counted once: LoadRegionIfPresent adds the region's
    // allocated count, so adopting a region already held would double it.
    const std::uint32_t after_first = peer->allocated_pages();
    auto again = peer->GetForRead(grown.value().first);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(peer->allocated_pages(), after_first);
    EXPECT_EQ(peer->map_refreshes_on_miss(), 1u);
}

TEST(DevicePageStoreOwnershipTest, TheSystemCoreDoesNotAdoptItsOwnMap) {
    // Core 0's copy **is** the free map, so a miss is an absence and there
    // is nothing to adopt. Pinned because the adoption is a device read on
    // an error path, and putting it on core 0's misses would price every
    // genuine NotFound the engine reports.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto absent = store->GetForRead(500);
    EXPECT_FALSE(absent.ok());
    EXPECT_EQ(absent.status().code(), StatusCode::kNotFound);
    EXPECT_EQ(store->map_refreshes_on_miss(), 0u);
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

TEST(DevicePageStoreTest, AReservationAfterTheLastFlushIsLandedByPersist) {
    // Found by PW3b's remount test (workplan-peer-writer.md §6): the store
    // marks its map dirty when `free_map_bytes()` is *taken*, so an
    // allocator holding that span across a flush reserved into a map the
    // next flush skipped as clean. Every extent refill core 0 granted after
    // its last flush reached the device only if something else dirtied the
    // map, and a peer's pages in one survived a restart only through redo's
    // CreateAt - which a checkpoint past their PAGE_INITs removes. Over the
    // store, a reservation marks the map and Persist() lands it: the grant
    // handler's call, made before the grant leaves.
    auto device = MakeDevice(64, /*initial_pages=*/256);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // **The allocator is built first, and the flush happens under it** -
    // production's shape (`Expeditor::Serve` builds it at startup and grants
    // refills for the rest of the run) and the only order that pins the
    // defect. Built after the flush, the constructor's own `free_map_bytes()`
    // would leave the map dirty, and a cached-span allocator would land its
    // reservation on that mark alone.
    ExtentAllocator extents(*store, /*hint=*/128);
    ASSERT_TRUE(store->Sync().ok());  // the map is clean - the shape at any refill
    auto lease = extents.Reserve(8);
    ASSERT_TRUE(lease.ok()) << lease.status().message();
    ASSERT_TRUE(extents.Persist().ok());

    store.reset();
    auto reopened = OpenStore(*device);
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(reopened->IsAllocated(lease.value().first))
        << "the reservation never reached the device";
    EXPECT_TRUE(reopened->IsAllocated(lease.value().end() - 1));
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

    ExtentAllocator extents(*store, /*hint=*/128);
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


// ---- The multi-page free map (FM2-FM5) --------------------------------
//
// docs/inflight/in-progress/workplan-multi-free-map.md, D1 settled as candidate A: region N is
// the ids [N*65280, (N+1)*65280), its free map at N*65280+1 and its
// headerless map at +2.

TEST(FreeMapRegionTest, ASingleRegionDatabaseTouchesOnlyRegionZerosMapIds) {
    // FM2's acceptance property in the form a test can hold: a database
    // that fits in one region touches the same map ids it always did, and
    // creates no third. Since FM6 it writes only the *free* map of those
    // two - the headerless bitmap's id stays reserved and its bytes are
    // never built, because nothing here is headerless.
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 5; ++i) {
        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        Fill(created.value().second.bytes(), static_cast<std::uint8_t>(i));
    }
    ASSERT_TRUE(store->Flush().ok());

    // Every map page that exists, and no others. Region 1's would be at
    // 65281/65282, and nothing should have gone near them.
    Page probe{};
    auto view = std::span<std::byte, kPageSize>(probe);
    ASSERT_TRUE(device->ReadPage(kFreeMapPageId, view).ok());
    EXPECT_EQ(RawPageType(view), static_cast<std::uint8_t>(PageType::kFreeMap));
    ASSERT_TRUE(device->ReadPage(kHeaderlessMapPageId, view).ok());
    EXPECT_EQ(RawPageType(view), static_cast<std::uint8_t>(PageType::kInvalid))
        << "a headerless bitmap was built for a database with no headerless page";
    EXPECT_LE(device->page_capacity(), kFreeMapBitsPerPage)
        << "a one-region database grew the file past region 0";

    // The headerless id is neither written nor allocated: the free map and
    // the 5 data pages are all that exist.
    EXPECT_EQ(store->allocated_pages(), 6u);
    EXPECT_FALSE(store->IsAllocated(kHeaderlessMapPageId));
}

TEST(FreeMapRegionTest, AllocationCrossesIntoANewRegionAndSkipsItsMapPages) {
    // FM5: walking off the end of region 0 creates region 1, whose own two
    // bitmap ids are marked in its own free map - so the next ids handed
    // out step over them.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    ASSERT_TRUE(store->RaiseAllocationFloor(kFreeMapBitsPerPage - 2).ok());

    const PageId region1 = FreeMapRegionBase(1);
    const std::vector<PageId> expected = {
        kFreeMapBitsPerPage - 2,  // the last two ids of region 0
        kFreeMapBitsPerPage - 1,
        region1,                  // region 1's first id; +1 and +2 are its maps
        region1 + 3,
        region1 + 4,
    };
    for (PageId want : expected) {
        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        EXPECT_EQ(created.value().first, want);
    }

    // The new region's free map exists and is allocated; its headerless
    // twin is neither, until something is headerless. **Both ids are
    // unplaceable either way** - that is arithmetic (IsMapPageId), not a
    // reserved bit, which is what makes the id safe to leave unmarked.
    EXPECT_TRUE(store->IsAllocated(FreeMapPageIdFor(region1)));
    EXPECT_FALSE(store->IsAllocated(HeaderlessMapPageIdFor(region1)));
    EXPECT_EQ(store->CreateAt(FreeMapPageIdFor(region1)).status().code(),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(store->CreateAt(HeaderlessMapPageIdFor(region1)).status().code(),
              StatusCode::kAlreadyExists);
}

TEST(FreeMapRegionTest, AGrownMapSurvivesARemount) {
    // The durability half: region 1's bitmaps must land, and the next mount
    // must load them - otherwise every page above region 0 reads as
    // unallocated and the data is silently unreachable.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    const PageId region1 = FreeMapRegionBase(1);
    PageId written = kInvalidPageId;

    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->RaiseAllocationFloor(kFreeMapBitsPerPage).ok());

        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        written = created.value().first;
        EXPECT_EQ(written, region1);
        Fill(created.value().second.bytes(), 42);
        ASSERT_TRUE(store->Flush().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->IsAllocated(written));
    EXPECT_TRUE(store->IsAllocated(FreeMapPageIdFor(region1)));

    auto got = store->GetForRead(written);
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_TRUE(Matches(got.value().bytes(), 42));

    // One free map per region, plus the one data page. No headerless
    // bitmap exists in either region.
    EXPECT_EQ(store->allocated_pages(), 3u);
}

TEST(FreeMapRegionTest, ATornMapPageRefusesTheMountRatherThanServingIt) {
    // FM9's rule, applied to what FM2 loads: RV3 converted a torn catalog
    // page from a mid-statement surprise into a refusal at the door, and a
    // map page is the same kind of fact about what exists.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    const PageId region1 = FreeMapRegionBase(1);
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->RaiseAllocationFloor(kFreeMapBitsPerPage).ok());
        ASSERT_TRUE(store->CreateNew().ok());
        ASSERT_TRUE(store->Flush().ok());
    }

    Page corrupt{};
    auto view = std::span<std::byte, kPageSize>(corrupt);
    ASSERT_TRUE(device->ReadPage(FreeMapPageIdFor(region1), view).ok());
    corrupt[kPageBodyOffset + 7] ^= std::byte{0x01};
    ASSERT_TRUE(device->WritePage(FreeMapPageIdFor(region1),
                                  std::span<const std::byte, kPageSize>(corrupt))
                    .ok());

    auto opened = DevicePageStore::Open(*device, 128);
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kCorruption);
}

TEST(FreeMapRegionTest, TheCeilingIsTheDesignCeilingNotOneBitmapPage) {
    // FM3. The four sites that read kFreeMapBitsPerPage as the size of the
    // id space now read kMaxPageCount, and a page above region 0 is an
    // ordinary page rather than an OutOfRange.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    const PageId above = FreeMapRegionBase(2) + 500;
    auto created = store->CreateAt(above);
    ASSERT_TRUE(created.ok()) << created.status().message();
    Fill(created.value().bytes(), 9);
    EXPECT_TRUE(store->IsAllocated(above));

    // The allocation floor moved with it. Equal is the legal terminal case
    // - "no id left" - which CreateNew already reports as OutOfSpace.
    EXPECT_EQ(store->RaiseAllocationFloor(kMaxPageCount + 1).code(), StatusCode::kOutOfRange);
    EXPECT_TRUE(store->RaiseAllocationFloor(kMaxPageCount).ok());
}

TEST(FreeMapRegionTest, ALeasedStoreGetsAPrivateRegionAndNeverReadsTheDevicesMap) {
    // A peer whose lease lies above region 0 must still be able to record a
    // headerless page **in memory** - that bit is what stops
    // StampIfHeadered writing a checksum over a headerless payload - while
    // reading core 0's live map page here would be an unsynchronised read
    // of a page core 0 is writing. So the region is private and empty.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    const PageId region1 = FreeMapRegionBase(1);
    LeasedIdSource lease(Extent{region1 + 100, 4});
    store->SetCoreOwnership(/*core_id=*/3, &lease, /*system_page_limit=*/128);

    auto created = store->CreateNewHeaderless();
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created.value().first, region1 + 100);
    EXPECT_TRUE(store->IsHeaderless(region1 + 100));

    // Nothing of the peer's reaches the device: FlushMaps drops a leased
    // store's map writes, as it always has.
    ASSERT_TRUE(store->Flush().ok());
    Page probe{};
    auto view = std::span<std::byte, kPageSize>(probe);
    ASSERT_TRUE(device->ReadPage(FreeMapPageIdFor(region1), view).ok());
    EXPECT_EQ(RawPageType(view), static_cast<std::uint8_t>(PageType::kInvalid))
        << "a peer published a free-map region";
}


// ---- FM6-FM11: the headerless map, grants, residency ------------------

TEST(FreeMapRegionTest, ADatabaseWithNoHeaderlessPageBuildsNoHeaderlessBitmap) {
    // FM6 / D2(a). waystone_dir.cpp is the engine's only creator of
    // headerless pages, so a database with no Waystone directory has none
    // anywhere - and a bitmap to record that costs a page of memory and a
    // page of mount I/O per region to say nothing.
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 3; ++i) ASSERT_TRUE(store->CreateNew().ok());
    ASSERT_TRUE(store->Flush().ok());

    const auto map = store->map_residency();
    EXPECT_EQ(map.regions, 1u);
    EXPECT_EQ(map.resident_pages, 1u) << "a headerless bitmap was built for nothing";
    EXPECT_FALSE(map.has_headerless);

    // Its id is not marked allocated either, so no allocated page is
    // unreadable - and it is still unreachable by allocation, because
    // every allocation path skips a bitmap id by arithmetic.
    EXPECT_FALSE(store->IsAllocated(kHeaderlessMapPageId));
    EXPECT_EQ(store->allocated_pages(), 4u);  // the free map + 3 data
    Page probe{};
    auto view = std::span<std::byte, kPageSize>(probe);
    ASSERT_TRUE(device->ReadPage(kHeaderlessMapPageId, view).ok());
    EXPECT_EQ(RawPageType(view), static_cast<std::uint8_t>(PageType::kInvalid));
}

TEST(FreeMapRegionTest, TheFirstHeaderlessPageBuildsTheBitmapAndItSurvives) {
    auto device = MakeDevice(/*extent_pages=*/8, /*initial_pages=*/0);
    PageId headerless = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        EXPECT_FALSE(store->map_residency().has_headerless);

        auto made = store->CreateNewHeaderless();
        ASSERT_TRUE(made.ok()) << made.status().message();
        headerless = made.value().first;
        EXPECT_TRUE(store->IsHeaderless(headerless));
        EXPECT_TRUE(store->map_residency().has_headerless);
        EXPECT_EQ(store->map_residency().resident_pages, 2u);
        ASSERT_TRUE(store->Flush().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->map_residency().has_headerless);
    EXPECT_TRUE(store->IsHeaderless(headerless))
        << "the headerless bitmap did not survive the remount";
    EXPECT_FALSE(store->IsHeaderless(kFreeMapPageId)) << "a bitmap page is headered";
}

TEST(FreeMapRegionTest, AGrantAboveRegionZeroIsHeldAndHonoured) {
    // D10(a). The rights bitmaps were single pages indexed by absolute id,
    // so they capped at 65,280 and a peer could hold no grant above region
    // 0 - GrantWritePages dropped the id at a range check and the refusal
    // then surfaced at MayFault, one layer from the cause.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    const PageId high = FreeMapRegionBase(3) + 777;
    LeasedIdSource lease(Extent{FreeMapRegionBase(1), 4});
    store->SetCoreOwnership(/*core_id=*/2, &lease, /*system_page_limit=*/128);

    EXPECT_FALSE(store->MayWrite(high));
    const PageId granted[] = {high};
    store->GrantWritePages(granted);
    EXPECT_TRUE(store->MayWrite(high)) << "a write grant above region 0 was dropped";
    EXPECT_TRUE(store->MayFault(high)) << "what a core may write it may read";
    EXPECT_FALSE(store->MayWrite(high + 1)) << "grants stay exact-page";

    // A fault grant spanning a region boundary keeps every id in it.
    const PageId across = FreeMapRegionBase(2) - 2;
    store->GrantFaultPages(Extent{across, 6});
    for (PageId id = across; id < across + 6; ++id) {
        EXPECT_TRUE(store->MayFault(id)) << "id " << id;
    }
    EXPECT_FALSE(store->MayFault(across + 6));
}

TEST(FreeMapRegionTest, TheAllocatedCountIsMaintainedNotSwept) {
    // D8(a). Every site that sets a free-map bit moves the count, and the
    // one that cannot report - the extent allocator, which writes through
    // the raw span - has a seam of its own.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->allocated_pages(), 1u);  // region 0's free map

    ASSERT_TRUE(store->CreateNew().ok());
    EXPECT_EQ(store->allocated_pages(), 2u);

    ExtentAllocator alloc(*store, 4096);
    auto e = alloc.Reserve(64);
    ASSERT_TRUE(e.ok()) << e.status().message();
    EXPECT_EQ(store->allocated_pages(), 66u) << "a leased run went uncounted";

    // Crossing into a new region adds that region's own free map, then the
    // page itself.
    ASSERT_TRUE(store->RaiseAllocationFloor(kFreeMapBitsPerPage).ok());
    ASSERT_TRUE(store->CreateNew().ok());
    EXPECT_EQ(store->allocated_pages(), 68u);
    EXPECT_EQ(store->map_residency().regions, 2u);

    // The first headerless page claims its bitmap's id as it places it -
    // in **region 1**, since that is where allocation now is, not region 0.
    // Two ids: the bitmap's own and the headerless page's.
    ASSERT_TRUE(store->CreateNewHeaderless().ok());
    EXPECT_EQ(store->allocated_pages(), 70u) << "the bitmap's own id went unclaimed";
    EXPECT_TRUE(store->IsAllocated(HeaderlessMapPageIdFor(FreeMapRegionBase(1))));
    EXPECT_FALSE(store->IsAllocated(kHeaderlessMapPageId))
        << "region 0 built a bitmap it has no headerless page for";
}

TEST(FreeMapRegionTest, AReservationNeverStraddlesARegion) {
    // D3(a), over a store rather than bare bytes: the run abandons the tail
    // of region 0 and restarts in region 1, past that region's two
    // reserved bitmap ids.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    ExtentAllocator alloc(*store, kFreeMapBitsPerPage - 10);
    auto e = alloc.Reserve(64);
    ASSERT_TRUE(e.ok()) << e.status().message();
    EXPECT_EQ(e.value().first, FreeMapRegionBase(1) + 3)
        << "the run straddled a region boundary or landed on a bitmap id";
    EXPECT_EQ(FreeMapRegionOf(e.value().first),
              FreeMapRegionOf(e.value().end() - 1))
        << "one reservation spans two regions";
}

TEST(FreeMapRegionTest, MapPagesAreNeverReclaimCandidates) {
    // FM8. Store-owned under D4(a), so they never enter the pool at all -
    // this is the guard against a future that puts them there, and it is
    // arithmetic so it holds without a resident frame to read a header off.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_TRUE(store->IsPinnedClass(kFreeMapPageId));
    EXPECT_TRUE(store->IsPinnedClass(kHeaderlessMapPageId));
    EXPECT_TRUE(store->IsPinnedClass(FreeMapPageIdFor(FreeMapRegionBase(9))));
    EXPECT_TRUE(store->IsPinnedClass(HeaderlessMapPageIdFor(FreeMapRegionBase(9))));
}

}  // namespace
}  // namespace kds::storage
