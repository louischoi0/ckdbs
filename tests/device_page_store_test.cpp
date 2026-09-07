#include "kds/base/current_core.hpp"
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

// ---- Core ownership: the system range (AM-R2, AO-R14) ----------------
//
// **The lease's cells went with the lease** (AW-S1b). A store bound to a
// non-system core used to allocate from a leased extent and never touch the
// free map, and a dozen cells pinned each half of that. One frame table
// serves every core now, so what is left of ownership in this class is the
// boundary below which only core 0 may write.

TEST(DevicePageStoreOwnershipTest, APeerMayNotWriteTheSystemRangeOnASharedStore) {
    // **The gate that stopped being one, and nothing failed when it did.**
    // `MayWrite` opened with `if (lease_ == nullptr) return true`, which
    // meant "core 0, which may write anything" while only a peer's store
    // carried a lease. AM-S2 step 3 made every core borrow core 0's store,
    // and the lease was only ever installed on an *owned* one - so from then
    // on every core reached this predicate with a null lease and was told
    // yes to everything, the system range included. AW-S1b then removed the
    // lease outright, which is why this is now the whole of the predicate:
    // `MayWrite` has four callers outside the store
    // (`mount_recovery.cpp`, `core_runtime.cpp`, `command_dispatcher.cpp`
    // twice) that read it as a real gate, and AM-R2 and AO-R14 both keep it
    // as one.
    //
    // **Mutation**: make the system arm `return true` and the peer arm
    // below answers true.
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    // **The production arrangement**: the system boundary installed
    // directly, which is what `Expeditor` does for core 0
    // (`SetResidentLimit(kFirstUserPageId)`). Before AW-a two members held
    // one boundary and only this one was set here, so `MayWrite`'s range
    // was 0 and its system arm was unreachable even before the null-lease
    // early return got to it. One boundary now, so installing it installs
    // both readings.
    constexpr PageId kSystemLimit = 128;
    store->SetResidentLimit(kSystemLimit);
    const PageId system_page = 4;
    const PageId user_page = 1000;

    // Core 0 writes anything, which is what it did before this repair and
    // after it.
    EXPECT_TRUE(store->MayWrite(system_page));
    EXPECT_TRUE(store->MayWrite(user_page));

    {
        // A peer on the same store. One writer per catalog page is the
        // property; a user page stays writable because routing to the
        // relation's owner is what gates that, not this predicate.
        const CurrentCoreGuard as_peer(3);
        EXPECT_FALSE(store->MayWrite(system_page))
            << "a peer was admitted to the system range on a shared store";
        EXPECT_TRUE(store->MayWrite(user_page));
    }

    // The identity is scoped, not sticky.
    EXPECT_TRUE(store->MayWrite(system_page));
}

TEST(DevicePageStoreOwnershipTest, ASharedStoreRefusesAPeersSystemWriteAndNotItsUserWrite) {
    // The cell above pins the *predicate*; this one pins what the predicate
    // is for. `MayWrite` has four callers outside this class, but the one
    // that stands between a peer and a torn catalog page is inside it -
    // `ResidentBytes`' `mark_dirty && !MayWrite(...)` gate - and the null
    // lease made that gate pass too. So the shared store gets the refusal
    // cell the leased store already has
    // (`AMissingGrantIsRetryableAndASystemPageIsNot`), with the same two
    // readings of the same two page ids.
    //
    // **Mutation**: restore `if (lease_ == nullptr) return true;` above the
    // system check and the system write is admitted.
    auto device = MakeDevice(64, 0);
    {
        // Core 0's half: the pages have to exist, or the refusal below
        // would be `NotFound` arriving before the gate rather than the gate.
        auto core0 = OpenStore(*device);
        ASSERT_NE(core0, nullptr);
        auto system_page = core0->CreateAt(4);
        ASSERT_TRUE(system_page.ok()) << system_page.status().message();
        FormatPage(system_page.value().bytes(), PageType::kHeap);
        auto relation_page = core0->CreateAt(130);
        ASSERT_TRUE(relation_page.ok()) << relation_page.status().message();
        FormatPage(relation_page.value().bytes(), PageType::kHeap);
        ASSERT_TRUE(core0->Sync().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    // The shared arrangement: the boundary installed directly, no lease -
    // `Expeditor::Open` and `CoreRuntime::Open` between them (AM-S2 step 3).
    store->SetResidentLimit(128);

    const CurrentCoreGuard as_peer(3);
    // Wrong now and wrong on every retry: the system range has one writer
    // for the life of the instance, so the code is the non-retryable one.
    auto refused = store->Get(4);
    ASSERT_FALSE(refused.ok()) << "a peer dirtied a system page on the shared pool";
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument)
        << refused.status().message();
    EXPECT_FALSE(refused.status().retryable());

    // And nothing more: a shared pool exists so that every core writes the
    // user pages through it. Routing to the relation's owner is what gates
    // that, not this predicate.
    auto admitted = store->Get(130);
    EXPECT_TRUE(admitted.ok()) << admitted.status().message();
}

TEST(DevicePageStoreTest, AnAllocatedPageNeverWrittenIsNotFoundNotCorrupt) {
    // Found by PW1c-7's restart test (workplan-peer-writer.md §8): the map
    // records an id the moment it is claimed, while the page's bytes reach
    // the device only at a write-back - so a crash between a page's
    // PAGE_INIT and its first flush leaves a page the map calls allocated
    // and the device holds as zeros. Reading it used to be a checksum
    // Corruption, which redo can only poison and wait for a full page image
    // to heal; as NotFound, redo's PAGE_INIT arm creates it and the mount
    // completes.
    //
    // **The crash is the point, and `PersistMaps` is what makes it one**:
    // it writes the bitmaps and syncs, so the claim is durable, and the
    // frame carrying the page's own bytes is still dirty in memory when the
    // store below goes away with it. The extent lease used to produce this
    // state for free - a run of 64 ids marked at reservation, written
    // lazily - and it was the arrangement that made it a *reported* defect;
    // one claim is the same state (AW-S1b).
    auto device = MakeDevice(64, /*initial_pages=*/256);
    PageId page = kInvalidPageId;
    {
        auto opened = OpenStore(*device);
        ASSERT_NE(opened, nullptr);
        auto created = opened->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        page = created.value().first;
        ASSERT_TRUE(opened->PersistMaps().ok());
    }

    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
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

TEST(FreeMapRegionTest, TheAllocatedCountIsMaintainedNotSwept) {
    // D8(a). Every site that sets a free-map bit moves the count. The one
    // site that could not report it - the extent allocator, which wrote
    // through the raw span and had a `NoteAllocated` seam for exactly this -
    // went with the leases (AW-S1b), so every writer of a bit is now inside
    // this class.
    auto device = MakeDevice(/*extent_pages=*/64, /*initial_pages=*/0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->allocated_pages(), 1u);  // region 0's free map

    ASSERT_TRUE(store->CreateNew().ok());
    EXPECT_EQ(store->allocated_pages(), 2u);

    // Crossing into a new region adds that region's own free map, then the
    // page itself.
    ASSERT_TRUE(store->RaiseAllocationFloor(kFreeMapBitsPerPage).ok());
    ASSERT_TRUE(store->CreateNew().ok());
    EXPECT_EQ(store->allocated_pages(), 4u);
    EXPECT_EQ(store->map_residency().regions, 2u);

    // The first headerless page claims its bitmap's id as it places it -
    // in **region 1**, since that is where allocation now is, not region 0.
    // Two ids: the bitmap's own and the headerless page's.
    ASSERT_TRUE(store->CreateNewHeaderless().ok());
    EXPECT_EQ(store->allocated_pages(), 6u) << "the bitmap's own id went unclaimed";
    EXPECT_TRUE(store->IsAllocated(HeaderlessMapPageIdFor(FreeMapRegionBase(1))));
    EXPECT_FALSE(store->IsAllocated(kHeaderlessMapPageId))
        << "region 0 built a bitmap it has no headerless page for";
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

// ---- The recovery pass writes without claiming (AR0 M0, AL-R6) ---------

// Under one stream core 0's mount pass replays and rolls back records
// belonging to every core, through core 0's store. Stamping core 0 onto a
// page core 2 owns mislabels whose stream may name it - and until AW-S1b it
// did worse: the next mount read that stamp to decide who may write, so
// core 2 faulted the page, was granted nothing, and could never write it
// again. The write half is gone; rule 5's "a page's stamp is the truth
// about its stream" is what the suppression still protects.
//
// Redo skips its own restamp, but undo's compensations reach the store
// through this same ordinary mutation path, which is why the suppression
// lives here rather than at either phase.
TEST(DevicePageStoreStampSuppressionTest, ASuppressedPassLeavesAnothersStampAlone) {
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    // Core 0 by default: the stamp is the calling thread's identity now, and
    // this thread declared none (AM-S2 step 3).

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    const PageId page = created.value().first;
    FormatPage(created.value().second.bytes(), PageType::kHeap);
    SetPageStreamStamp(created.value().second.bytes(), StreamStampFor(2));  // core 2's

    store->SetStampSuppressed(true);
    ASSERT_TRUE(store->StampPageLsn(page, 4096).ok());
    store->SetStampSuppressed(false);

    auto got = store->Get(page);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(GetPageStreamStamp(got.value().bytes()), StreamStampFor(2))
        << "the recovery pass claimed a page it was only replaying";
    // The page_lsn is still stamped: idempotence is that field's job and it
    // is not ownership.
    EXPECT_EQ(GetPageLsn(got.value().bytes()), 4096u);
}

// And with the suppression off - every path a serving instance takes - the
// stamp is written exactly as before.
TEST(DevicePageStoreStampSuppressionTest, AnOrdinaryWriteStillClaimsThePage) {
    auto device = MakeDevice(64, 0);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    // The stamp records the *calling* core since AM-S2 step 3, so the cell
    // declares one rather than telling the store to pretend.
    const CurrentCoreGuard as_core_two(2);
    EXPECT_FALSE(store->stamp_suppressed());

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok());
    const PageId page = created.value().first;
    FormatPage(created.value().second.bytes(), PageType::kHeap);

    ASSERT_TRUE(store->StampPageLsn(page, 4096).ok());

    auto got = store->Get(page);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(GetPageStreamStamp(got.value().bytes()), StreamStampFor(2));
}

}  // namespace
}  // namespace kds::storage
