// DO's E2 (`instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
// §5a, DO-R1): an armed store walks the checkpoint's dirty table a slot
// chunk per structure-latch hold, so the table is each frame's as of its
// chunk's hold and not one instant's. These cells pin what §5a's proof
// rests on and what it claims:
//
//   - the merge keeps a page once, at its lowest real recLSN, never letting
//     a recLSN of 0 hide a real one (§5a case 5);
//   - armed, the walk crosses chunk boundaries with the latch released (the
//     seam runs there), and a page written after its chunk was walked is
//     absent from the table - the window §5a's case 4 names;
//   - unarmed it is one walk, as before (DO-R2);
//   - **the crash cells**: a row updated, logged and committed between a
//     walked chunk and `CHECKPOINT_BEGIN` survives a crash before the run's
//     anchor lands (the previous anchor replays it) and after it (the
//     publish wrote its page). The update runs on the walking thread, which
//     §5a's fold argument says no checkpointing core does; neither cover
//     these cells reach depends on who appended, and nothing here
//     exercises the fold. The second cell was shown to fail when the
//     publish writes no page but page 0: redo starts past the update and the
//     row reads its old value (DO-S3, the mutant recorded in the order).

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "sim/instance.hpp"

namespace kds::storage {
namespace {

using Table = std::vector<std::pair<PageId, wal::Lsn>>;

TEST(DirtyTableMergeTest, APageMetTwiceKeepsItsLowestRealRecLsn) {
    Table table = {{9, 300}, {4, 0}, {9, 0}, {4, 0}, {7, 50}, {9, 120}, {7, 80}};
    DevicePageStore::MergeDirtyTable(table);
    const Table want = {{4, 0}, {7, 50}, {9, 120}};
    EXPECT_EQ(table, want) << "a zero recLSN hid a real one, or a page stayed twice";
}

class DirtyTableChunkedWalkTest : public ::testing::Test {
protected:
    // `pages` formatted pages created and left dirty, in a store of
    // `capacity` frames, armed or not.
    void Fill(std::size_t pages, std::size_t capacity, bool armed) {
        device_ = std::move(MemoryPageDevice::Create(/*extent_pages=*/64, 0).value());
        auto store = DevicePageStore::Open(*device_, FrameCapacity{capacity}, /*first=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetLatchArmed(armed);
        for (std::size_t i = 0; i < pages; ++i) {
            auto created = store_->CreateNew();
            ASSERT_TRUE(created.ok()) << created.status().message();
            FormatPage(created.value().second.bytes(), PageType::kHeap);
            ids_.push_back(created.value().first);
        }
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
    std::vector<PageId> ids_;
};

TEST_F(DirtyTableChunkedWalkTest, AnArmedWalkCrossesEveryChunkAndListsEveryDirtyPageOnce) {
    Fill(/*pages=*/2500, /*capacity=*/4096, /*armed=*/true);
    std::size_t gaps = 0;
    store_->SetAfterDirtyTableChunkForTest([&](std::size_t) { ++gaps; });
    const Table table = store_->DirtyPagesWithRecLsn();
    EXPECT_EQ(gaps, 3u) << "2,500 resident frames span three 1,024-slot chunks";
    std::vector<PageId> listed;
    for (const auto& [id, rec_lsn] : table) listed.push_back(id);
    EXPECT_EQ(listed, store_->DirtyPageIds());
    for (const PageId id : ids_) {
        EXPECT_TRUE(std::binary_search(listed.begin(), listed.end(), id)) << id;
    }
}

TEST_F(DirtyTableChunkedWalkTest, UnarmedItIsOneWalkWithNoGap) {
    // Under the census run every store is armed at Open() and SetLatchArmed
    // cannot undo it, so the unarmed walk does not exist there: skipped, not
    // faked (as `PageLatchStoreTest`'s unarmed cell).
    if (std::getenv("KDS_TEST_PAGE_LATCH") != nullptr) {
        GTEST_SKIP() << "the census override arms every store; this cell is about the unarmed walk";
    }
    Fill(/*pages=*/2500, /*capacity=*/4096, /*armed=*/false);
    std::size_t gaps = 0;
    store_->SetAfterDirtyTableChunkForTest([&](std::size_t) { ++gaps; });
    EXPECT_EQ(store_->DirtyPageIds().size(), store_->DirtyPagesWithRecLsn().size());
    EXPECT_EQ(gaps, 0u) << "an unarmed store walked in chunks (DO-R2)";
}

TEST_F(DirtyTableChunkedWalkTest, APageWrittenAfterItsChunkWasWalkedIsNotInTheTable) {
    Fill(/*pages=*/2500, /*capacity=*/4096, /*armed=*/true);
    ASSERT_TRUE(store_->Flush().ok());  // every page clean
    ASSERT_TRUE(store_->DirtyPageIds().empty());
    // Dirtied and stamped once the first chunk is walked: if its slot was in
    // that chunk, the table cannot carry it - §5a case 4's window, which the
    // publish and the fold cover and the crash cells below exercise.
    PageId written = kInvalidPageId;
    store_->SetAfterDirtyTableChunkForTest([&](std::size_t chunk) {
        if (chunk != 0 || written != kInvalidPageId) return;
        // The first created page took the first chunk's first slot; the
        // stamp dirties it.
        written = ids_.front();
        ASSERT_TRUE(store_->StampPageLsn(written, 4096).ok());
    });
    const Table table = store_->DirtyPagesWithRecLsn();
    ASSERT_NE(written, kInvalidPageId);
    EXPECT_TRUE(table.empty()) << "the page written after its chunk was walked is in the table";
    store_->SetAfterDirtyTableChunkForTest(nullptr);
    EXPECT_EQ(store_->DirtyPageIds(), std::vector<PageId>{written}) << "the next walk lists it";
}

// ---- The crash cells (DO-R1) ----------------------------------------------

class DirtyTableCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        sim::SimInstance::Options options;
        options.buffer_pool_frames = 4096;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
        ASSERT_EQ(db_->Execute("CREATE TABLE t (id int64, v int64)").rfind("ERR", 0),
                  std::string::npos);
        for (int i = 1; i <= 50; ++i) {
            const std::string reply = db_->Execute("INSERT INTO t VALUES (" + std::to_string(i) +
                                                   ", " + std::to_string(i) + ")");
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        }
        // The previous anchor, below everything the cell writes next, and
        // every page clean, so the cell's row is the only one the table
        // could miss.
        ASSERT_TRUE(db_->RunCheckpoint().ok());
        ASSERT_EQ(db_->Execute("SYNC").rfind("ERR", 0), std::string::npos);
        // The walk in chunks is the armed store's; one thread drives it.
        db_->store().SetLatchArmed(true);
        // One chunk, so the seam runs once the row's page is walked: the
        // cells exercise the window between the walk and the append, which
        // a one-hold walk also has; that E2 widens it to "after the page's
        // chunk" is the store cell above's to show.
        ASSERT_LE(db_->store().frame_slots().slots, DevicePageStore::kFrameChunk);
    }

    // The seam's write: row 7 updated and committed after the first chunk
    // of the run's first walk - its record below the run's
    // `CHECKPOINT_BEGIN`, its page absent from the run's table.
    void UpdateInTheWindow() {
        db_->store().SetAfterDirtyTableChunkForTest([this](std::size_t chunk) {
            if (chunk != 0 || updated_) return;
            updated_ = true;
            const std::string reply = db_->Execute("UPDATE t SET v = 7007 WHERE id = 7");
            EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        });
    }

    std::string ValueOfRow7() { return db_->Execute("SELECT v FROM t WHERE id = 7"); }

    std::unique_ptr<sim::SimInstance> db_;
    bool updated_ = false;
};

TEST_F(DirtyTableCrashTest, ACrashBeforeTheRunsAnchorReplaysTheWindowsWriteFromThePreviousOne) {
    UpdateInTheWindow();
    ASSERT_TRUE(db_->AbandonCheckpointMidSteps().ok());  // BEGIN and a step; no anchor
    ASSERT_TRUE(updated_) << "the seam did not run: the walk was not in chunks";
    db_->store().SetAfterDirtyTableChunkForTest(nullptr);
    db_->Crash();
    ASSERT_TRUE(db_->Reboot().ok());
    EXPECT_NE(ValueOfRow7().find("7007"), std::string::npos) << ValueOfRow7();
}

TEST_F(DirtyTableCrashTest, ACrashAfterTheRunsAnchorFindsTheWindowsWriteOnThePage) {
    UpdateInTheWindow();
    ASSERT_TRUE(db_->RunCheckpoint().ok());  // the publish writes it before the anchor
    ASSERT_TRUE(updated_) << "the seam did not run: the walk was not in chunks";
    db_->store().SetAfterDirtyTableChunkForTest(nullptr);
    db_->Crash();
    ASSERT_TRUE(db_->Reboot().ok());
    EXPECT_NE(ValueOfRow7().find("7007"), std::string::npos) << ValueOfRow7();
}

}  // namespace
}  // namespace kds::storage
