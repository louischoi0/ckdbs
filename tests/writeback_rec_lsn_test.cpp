#include "kds/storage/device_page_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

// A writeback whose copy a write overtakes leaves the frame dirty, and its
// recLSN moves up to the lowest LSN stamped since the copy
// (`DevicePageStore::WriteBack`, `Frame::stamped_since_copy`). Before it, the
// frame kept its first recLSN across every overtaken copy, so one page
// written continuously pinned the instance's redo start and the log stopped
// recycling.
//
// The recLSN is a crash claim: every change missing from the device's image
// of the page carries an LSN at or above it, so redo from it restores them.
// `ExpectRecLsnCoversTheDevice` checks exactly that against the device as a
// crash would leave it - each change is a byte of its own at a body offset
// of its own, so the image says which changes went out.

namespace kds::storage {
namespace {

class WritebackRecLsnTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto pages = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
        ASSERT_TRUE(pages.ok());
        device_ = std::move(pages.value());
        auto store = DevicePageStore::Open(*device_, FrameCapacity{512},
                                           /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        auto made = store_->CreateNew();
        ASSERT_TRUE(made.ok());
        page_ = made.value().first;
        FormatPage(made.value().second.bytes(), PageType::kHeap);
        made.value().second.Release();
        // Clean and on the device, so every change below is one it lacks.
        WriteBack();
        ASSERT_EQ(RecLsn(), std::nullopt);
    }

    void TearDown() override {
        if (store_ != nullptr) store_->SetAfterWritebackCopyForTest({});
    }

    // Change `k`: its own body byte, written under the exclusive hold, then
    // stamped at `lsn` - a mutation path's order.
    void Change(std::size_t k, wal::Lsn lsn) {
        Mutate(k);
        Stamp(k, lsn);
    }
    void Mutate(std::size_t k) {
        auto ref = store_->Get(page_);
        ASSERT_TRUE(ref.ok());
        ref.value().bytes()[kPageBodyOffset + k] = Mark(k);
    }
    void Stamp(std::size_t k, wal::Lsn lsn) {
        ASSERT_TRUE(store_->StampPageLsn(page_, lsn).ok());
        lsns_[k] = lsn;
    }

    void WriteBack() {
        const std::array<PageId, 1> ids{page_};
        ASSERT_TRUE(store_->WriteBackPages(ids, nullptr).ok());
    }
    // `hook` runs between the copy and the clean of every writeback of the
    // page, as the write that overtakes the copy.
    void WriteBackRacedBy(std::function<void()> hook) {
        store_->SetAfterWritebackCopyForTest([&](PageId copied) {
            if (copied == page_) hook();
        });
        WriteBack();
        store_->SetAfterWritebackCopyForTest({});
    }

    std::optional<wal::Lsn> RecLsn() const {
        for (const auto& [id, rec_lsn] : store_->DirtyPagesWithRecLsn()) {
            if (id == page_) return rec_lsn;
        }
        return std::nullopt;
    }

    // Every change the device's image lacks is at or above the recLSN, and
    // a page with any such change is dirty.
    void ExpectRecLsnCoversTheDevice() {
        std::array<std::byte, kPageSize> image{};
        ASSERT_TRUE(device_->ReadPage(page_, image).ok());
        const std::optional<wal::Lsn> rec_lsn = RecLsn();
        for (std::size_t k = 0; k < lsns_.size(); ++k) {
            if (lsns_[k] == wal::kNoLsn || image[kPageBodyOffset + k] == Mark(k)) continue;
            ASSERT_TRUE(rec_lsn.has_value()) << "change " << k << " is lost and the page is clean";
            EXPECT_LE(*rec_lsn, lsns_[k])
                << "change " << k << " at LSN " << lsns_[k] << " is not on the device, and redo "
                << "from recLSN " << *rec_lsn << " would not replay it";
        }
    }

    static std::byte Mark(std::size_t k) { return static_cast<std::byte>(k + 1); }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
    PageId page_ = kInvalidPageId;
    std::array<wal::Lsn, 64> lsns_{};
};

TEST_F(WritebackRecLsnTest, AStampAfterTheCopyMovesTheRecLsnUpToIt) {
    Change(0, 100);
    WriteBackRacedBy([&] { Change(1, 200); });
    EXPECT_EQ(RecLsn(), 200u) << "the image carries change 0; only change 1 needs redo";
    ExpectRecLsnCoversTheDevice();

    WriteBack();
    EXPECT_EQ(RecLsn(), std::nullopt);
    ExpectRecLsnCoversTheDevice();
}

TEST_F(WritebackRecLsnTest, APageOvertakenAtEveryCopyNoLongerKeepsItsFirstRecLsn) {
    // The bug's liveness case: a hot page whose copy is overtaken by every
    // writeback. Its recLSN follows the writes instead of pinning the first.
    Change(0, 100);
    for (std::size_t round = 1; round <= 20; ++round) {
        const wal::Lsn lsn = 100 + 50 * round;
        WriteBackRacedBy([&] { Change(round, lsn); });
        EXPECT_EQ(RecLsn(), lsn) << "round " << round;
        ExpectRecLsnCoversTheDevice();
    }
}

TEST_F(WritebackRecLsnTest, AWriteWithNoStampSinceTheCopyKeepsTheRecLsn) {
    // A write fetched after the copy and not yet stamped moves the
    // generation, so the clean is skipped, and there is no LSN to move to.
    Change(0, 100);
    WriteBackRacedBy([&] { Mutate(1); });
    EXPECT_EQ(RecLsn(), 100u);
    // Its stamp, after the clean, at a higher LSN than the old recLSN.
    Stamp(1, 300);
    EXPECT_EQ(RecLsn(), 100u);
    ExpectRecLsnCoversTheDevice();
}

TEST_F(WritebackRecLsnTest, ARecordAppendedBeforeAHigherStampIsCoveredWhenStampedAfterTheCopy) {
    // A stamp below the copy's page_lsn, after the copy (wal.md §11-2): a
    // record logged after its page is released can land that way. "Just
    // above the copy's page_lsn" would set the recLSN to L2 + 1 and lose L1
    // to the recycler.
    constexpr wal::Lsn kL1 = 200;
    constexpr wal::Lsn kL2 = 300;
    Change(0, 100);
    Change(1, kL2);
    WriteBackRacedBy([&] { Change(2, kL1); });
    EXPECT_EQ(RecLsn(), kL1);
    ExpectRecLsnCoversTheDevice();
}

TEST_F(WritebackRecLsnTest, ALowerStampAfterTheSkippedCleanLowersTheRecLsn) {
    // A lower stamp after the clean: the recLSN is the minimum of every
    // stamp since, not the first of them.
    Change(0, 100);
    WriteBackRacedBy([&] { Change(1, 300); });
    ASSERT_EQ(RecLsn(), 300u);
    Change(2, 250);
    EXPECT_EQ(RecLsn(), 250u);
    ExpectRecLsnCoversTheDevice();
}

TEST_F(WritebackRecLsnTest, ALowerStampAfterACleanLowersTheRecLsn) {
    // Out of order from a clean frame too: first-wins kept 300, and redo
    // from it would skip 250.
    Change(0, 300);
    Change(1, 250);
    EXPECT_EQ(RecLsn(), 250u);
    ExpectRecLsnCoversTheDevice();
}

}  // namespace
}  // namespace kds::storage
