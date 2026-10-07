// **BF-S2: the free primitive and the allocator** (BF-R5, BF-R6;
// `instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`).
//
// `DevicePageStore::FreePage` discards a page's frame, dirty or not, and
// clears its bit in one hold; it defers a held frame and refuses an id no
// relation owns. `CreateNew` hands a freed id out again ahead of the
// cursor, skipping one with a fault in flight; `CreateNewHeaderless` never
// takes one. `FlushMaps` carries a write barrier, so a `PersistMaps` that
// returns covers another core's copied map state. No caller frees anything
// yet - BF-S3 adds the reclaim - so these cells drive the store directly.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/stats/instance_key.hpp"
#include "kds/stats/waystone_dir.hpp"

namespace kds::storage {
namespace {

using FreeOutcome = PageStore::FreeOutcome;

// Waits up to ten seconds for `flag`: a seam that never fires is a failed
// cell, not a hung one - which is also what lets a mutation that stops it
// firing be counted as killed.
bool SpinUntil(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

constexpr PageId kFirstNew = 16;

class PageFreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        Reopen();
    }

    // A store over the same device: what a clean restart leaves, once
    // the old one has synced.
    void Reopen() {
        store_.reset();
        auto store = DevicePageStore::Open(*device_, kFirstNew);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
    }

    // A heap page carrying `fill` in its body. Left dirty unless `sync`.
    PageId MakePage(std::byte fill, bool sync = true) {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        const PageId id = created.value().first;
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = fill;
        created.value().second.Release();
        if (sync) EXPECT_TRUE(store_->Sync().ok());
        return id;
    }

    // The device's bytes for `id`, read past the store.
    std::array<std::byte, kPageSize> DeviceBytes(PageId id) {
        std::array<std::byte, kPageSize> out{};
        EXPECT_TRUE(device_->ReadPage(id, std::span<std::byte, kPageSize>(out)).ok());
        return out;
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

TEST_F(PageFreeTest, AFreedIdHasNoFrameAndItsNextCreateIsAZeroedPage) {
    const PageId id = MakePage(std::byte{0x5A});
    const std::uint32_t allocated = store_->allocated_pages();
    const std::size_t resident = store_->resident_pages();

    auto freed = store_->FreePage(id);
    ASSERT_TRUE(freed.ok()) << freed.status().message();
    EXPECT_EQ(freed.value(), FreeOutcome::kFreed);
    EXPECT_FALSE(store_->IsAllocated(id));
    EXPECT_EQ(store_->allocated_pages(), allocated - 1);
    EXPECT_EQ(store_->resident_pages(), resident - 1) << "the frame went with the bit";
    EXPECT_EQ(store_->Get(id).status().code(), StatusCode::kNotFound);
    EXPECT_EQ(store_->pages_freed(), 1u);

    // The list is asked ahead of the cursor, and the page it hands back is
    // a creation: zeroed, not the dead page's bytes.
    auto again = store_->CreateNew();
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value().first, id);
    const auto bytes = again.value().second.bytes();
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; }));
    EXPECT_EQ(store_->pages_reused(), 1u);
    EXPECT_EQ(store_->allocated_pages(), allocated);
}

TEST_F(PageFreeTest, ADirtyFrameIsDiscardedAndNeverWrittenBack) {
    const PageId id = MakePage(std::byte{0x11});
    {
        auto page = store_->Get(id);
        ASSERT_TRUE(page.ok());
        page.value().bytes()[kPageBodyOffset] = std::byte{0x22};
    }
    const std::vector<PageId> before = store_->DirtyPageIds();
    ASSERT_NE(std::find(before.begin(), before.end(), id), before.end());

    ASSERT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed);
    const std::vector<PageId> dirty = store_->DirtyPageIds();
    EXPECT_EQ(std::find(dirty.begin(), dirty.end(), id), dirty.end());
    ASSERT_TRUE(store_->Sync().ok());
    EXPECT_EQ(DeviceBytes(id)[kPageBodyOffset], std::byte{0x11})
        << "the dirty bytes were written back after the free";
}

TEST_F(PageFreeTest, AHeldFrameDefersTheFreeAndChangesNothing) {
    const PageId id = MakePage(std::byte{1});
    const std::uint32_t allocated = store_->allocated_pages();

    {
        auto pinned = store_->GetForRead(id);
        ASSERT_TRUE(pinned.ok());
        EXPECT_EQ(store_->FreePage(id).value(), FreeOutcome::kDeferred) << "pinned";
        EXPECT_TRUE(store_->IsAllocated(id));
        EXPECT_EQ(store_->allocated_pages(), allocated);
    }

    store_->SetLatchArmed(true);
    ASSERT_TRUE(store_->LatchFrameForTest(id, PinMode::kShared, /*core=*/7).ok());
    EXPECT_EQ(store_->FreePage(id).value(), FreeOutcome::kDeferred) << "latched by another core";
    ASSERT_TRUE(store_->UnlatchFrameForTest(id, /*core=*/7).ok());

    // A writeback's claim: the hook runs between its copy and its clean,
    // holding the claim and no latch.
    {
        auto page = store_->Get(id);
        ASSERT_TRUE(page.ok());
        page.value().bytes()[kPageBodyOffset] = std::byte{2};
    }
    std::atomic<int> deferred_under_claim{0};
    store_->SetAfterWritebackCopyForTest([&](PageId copied) {
        if (copied != id) return;
        auto outcome = store_->FreePage(id);
        if (outcome.ok() && outcome.value() == FreeOutcome::kDeferred) ++deferred_under_claim;
    });
    ASSERT_TRUE(store_->Sync().ok());
    store_->SetAfterWritebackCopyForTest(nullptr);
    EXPECT_EQ(deferred_under_claim.load(), 1) << "claimed by a writeback";

    EXPECT_TRUE(store_->IsAllocated(id));
    EXPECT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed) << "released, the free proceeds";
}

TEST_F(PageFreeTest, AnIdNoRelationOwnsIsRefusedAndASecondFreeIsNotFound) {
    auto refused = store_->FreePage(kFirstNew - 1);
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption) << "the system range";
    refused = store_->FreePage(FreeMapPageIdFor(kFreeMapBitsPerPage));
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption) << "a map page";

    auto headerless = store_->CreateNewHeaderless();
    ASSERT_TRUE(headerless.ok());
    const PageId directory = headerless.value().first;
    headerless.value().second.Release();
    refused = store_->FreePage(directory);
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption) << "a headerless page";
    EXPECT_TRUE(store_->IsAllocated(directory));

    auto bound = store_->CreateNew();
    ASSERT_TRUE(bound.ok());
    const PageId cabin = bound.value().first;
    FormatPage(bound.value().second.bytes(), PageType::kCabinBound);
    bound.value().second.Release();
    refused = store_->FreePage(cabin);
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption) << "a Bound Cabin page";
    EXPECT_TRUE(store_->IsAllocated(cabin));

    const PageId id = MakePage(std::byte{3});
    ASSERT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed);
    EXPECT_EQ(store_->FreePage(id).status().code(), StatusCode::kNotFound);
    EXPECT_EQ(store_->pages_freed(), 1u);
}

TEST_F(PageFreeTest, TheListIsAskedAheadOfTheCursor) {
    const PageId a = MakePage(std::byte{1});
    const PageId b = MakePage(std::byte{2});
    const PageId c = MakePage(std::byte{3});
    ASSERT_LT(a, b);
    ASSERT_LT(b, c);
    ASSERT_EQ(store_->FreePage(b).value(), FreeOutcome::kFreed);

    auto first = store_->CreateNew();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().first, b) << "the cursor stands past c";
    first.value().second.Release();
    auto second = store_->CreateNew();
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second.value().first, c) << "the list is empty again; the cursor serves";
    EXPECT_EQ(store_->pages_reused(), 1u);
}

TEST_F(PageFreeTest, AHeaderlessCreateNeverTakesAFreedIdAndZeroesADeadImageAfterARestart) {
    const PageId a = MakePage(std::byte{0x7E});
    ASSERT_EQ(store_->FreePage(a).value(), FreeOutcome::kFreed);

    // In the run that freed it: the list is skipped.
    auto headerless = store_->CreateNewHeaderless();
    ASSERT_TRUE(headerless.ok());
    EXPECT_NE(headerless.value().first, a);
    headerless.value().second.Release();
    ASSERT_TRUE(store_->Sync().ok());

    // After a clean restart the cursor starts low and finds `a`'s cleared
    // bit, with the dead image still on the device. The create zeroes it on
    // the device before the id is handed out (Census A, R1).
    ASSERT_NE(DeviceBytes(a)[kPageBodyOffset], std::byte{0});
    Reopen();
    auto landed = store_->CreateNewHeaderless();
    ASSERT_TRUE(landed.ok());
    ASSERT_EQ(landed.value().first, a) << "the cursor's lowest clear bit";
    const auto device = DeviceBytes(a);
    EXPECT_TRUE(std::all_of(device.begin(), device.end(), [](std::byte b) { return b == std::byte{0}; }))
        << "the dead image is still on the device under a headerless page";
}

TEST_F(PageFreeTest, APopSkipsAnIdWhoseFaultIsInFlight) {
    // A stale fault of a freed id: its bit is clear, so it will answer
    // NotFound - but between publishing to `loading_` and reading it holds
    // the id, and a create of that id in the window would race the dead
    // image into the new page's frame.
    store_->SetLatchArmed(true, /*concurrent_pinners=*/2);
    const PageId id = MakePage(std::byte{9});
    ASSERT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed);

    std::atomic<bool> parked{false};
    std::atomic<bool> release{false};
    store_->SetAfterFaultPublishedForTest([&](PageId faulting) {
        if (faulting != id) return;
        parked = true;
        while (!release) std::this_thread::yield();
    });
    std::thread stale([&] {
        auto read = store_->GetForRead(id);
        EXPECT_EQ(read.status().code(), StatusCode::kNotFound);
    });
    if (!SpinUntil(parked)) {
        release = true;
        stale.join();
        FAIL() << "the seam never fired";
    }

    auto created = store_->CreateNew();
    ASSERT_TRUE(created.ok());
    EXPECT_NE(created.value().first, id) << "the pop handed out an id a fault holds";
    created.value().second.Release();

    release = true;
    stale.join();
    store_->SetAfterFaultPublishedForTest(nullptr);
    auto later = store_->CreateNew();
    ASSERT_TRUE(later.ok());
    EXPECT_EQ(later.value().first, id) << "the fault is over; the id is the list's again";
}

TEST_F(PageFreeTest, AFaultInFlightDefersTheFree) {
    // The id is allocated and its fault is between publishing to `loading_`
    // and reading: the bytes it will insert are the page's, so freeing under
    // it would leave a frame on a clear bit.
    store_->SetLatchArmed(true, /*concurrent_pinners=*/2);
    const PageId id = MakePage(std::byte{5});
    Reopen();  // no frame: the read below is a genuine fault
    store_->SetLatchArmed(true, /*concurrent_pinners=*/2);

    std::atomic<bool> parked{false};
    std::atomic<bool> release{false};
    store_->SetAfterFaultPublishedForTest([&](PageId faulting) {
        if (faulting != id) return;
        parked = true;
        while (!release) std::this_thread::yield();
    });
    std::thread fault([&] { EXPECT_TRUE(store_->GetForRead(id).ok()); });
    if (!SpinUntil(parked)) {
        release = true;
        fault.join();
        FAIL() << "the seam never fired";
    }

    EXPECT_EQ(store_->FreePage(id).value(), FreeOutcome::kDeferred);
    EXPECT_TRUE(store_->IsAllocated(id));

    release = true;
    fault.join();
    store_->SetAfterFaultPublishedForTest(nullptr);
    EXPECT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed) << "the fault is over";
}

TEST_F(PageFreeTest, TheCursorPassesOverAListedIdAboveIt) {
    // A clean mount puts the cursor below every page; a page freed above it
    // is the list's, so neither the cursor nor a headerless create takes it
    // (the BF-S2 review's B2).
    const PageId a = MakePage(std::byte{1});
    const PageId b = MakePage(std::byte{2});
    const PageId c = MakePage(std::byte{3});
    Reopen();
    ASSERT_EQ(store_->FreePage(c).value(), FreeOutcome::kFreed);

    auto headerless = store_->CreateNewHeaderless();
    ASSERT_TRUE(headerless.ok());
    EXPECT_NE(headerless.value().first, c) << "the cursor took a listed id";
    EXPECT_GT(headerless.value().first, c);
    headerless.value().second.Release();
    auto created = store_->CreateNew();
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().first, c) << "the list serves it";
    (void)a;
    (void)b;
}

TEST_F(PageFreeTest, ADirectorySlotThatReadsZeroIsEmpty) {
    // Census A, A1: an interior page whose frame never reached the device
    // reads as zeros after a crash - a never-written id, or one zeroed over
    // a reclaimed page's dead image. Zero is the superblock's id, never a
    // child: the walk must read it as an empty slot, not follow it.
    auto root = stats::CreateDirPage(*store_);
    ASSERT_TRUE(root.ok()) << root.status().message();
    {
        auto page = store_->Get(root.value());
        ASSERT_TRUE(page.ok());
        std::fill(page.value().bytes().begin(), page.value().bytes().end(), std::byte{0});
    }
    const stats::InstanceKey key{/*fetch_id=*/1, /*arg_hash=*/42};
    auto found = stats::LookupWaystonePage(*store_, root.value(), /*depth=*/2, key);
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), kInvalidPageId);

    auto made = stats::LookupOrCreateWaystonePage(*store_, root.value(), /*depth=*/2, key);
    ASSERT_TRUE(made.ok()) << made.status().message();
    EXPECT_GE(made.value(), kFirstNew) << "a fresh page, linked where the zero stood";
}

TEST_F(PageFreeTest, APersistMapsReturnsOnlyAfterAnotherCoresCopiedMapIsWritten) {
    // Without the barrier the second caller finds every region clean - the
    // first copied and cleared them - syncs nothing new, and returns while
    // the first is still between its copy and its write.
    store_->SetLatchArmed(true, /*concurrent_pinners=*/2);
    const PageId id = MakePage(std::byte{4});
    ASSERT_EQ(store_->FreePage(id).value(), FreeOutcome::kFreed);

    std::atomic<bool> parked{false};
    std::atomic<bool> release{false};
    store_->SetAfterMapCopyForTest([&] {
        if (parked.exchange(true)) return;  // the first flush parks; the second does not
        while (!release) std::this_thread::yield();
    });
    std::thread first([&] { EXPECT_TRUE(store_->PersistMaps().ok()); });
    if (!SpinUntil(parked)) {
        release = true;
        first.join();
        FAIL() << "the seam never fired";
    }

    std::atomic<bool> second_returned{false};
    std::thread second([&] {
        EXPECT_TRUE(store_->PersistMaps().ok());
        second_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(second_returned.load()) << "returned before the first core's copy was written";

    release = true;
    first.join();
    second.join();
    store_->SetAfterMapCopyForTest(nullptr);
    EXPECT_TRUE(second_returned.load());

    // And what it covered is on the device: the cleared bit.
    std::array<std::byte, kPageSize> map{};
    ASSERT_TRUE(device_->ReadPage(FreeMapPageIdFor(id), std::span<std::byte, kPageSize>(map)).ok());
    EXPECT_FALSE(FreeMapIsAllocated(std::span<const std::byte, kPageSize>(map), FreeMapBitIndexOf(id)));
}

}  // namespace
}  // namespace kds::storage
