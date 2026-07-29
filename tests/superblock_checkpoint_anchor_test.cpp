#include "kds/server/superblock_checkpoint_anchor.hpp"

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_mgr/checkpoint_target.hpp"
#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"

// The point of a durable anchor is one thing: the redo start a checkpoint
// computed is still there after the process is gone (wal.md sections 11-3
// and 14-3). Every test below is a variation on "encode it, throw the
// SuperBlock away, decode the page again".

namespace kds::server {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;

class NoTxns final : public wal::ActiveTransactions {
public:
    std::vector<std::uint64_t> Snapshot() const override { return {}; }
};

// Fails Sync() on demand, so the "the anchor did not land" path is a real
// path and not a comment.
class UnsyncablePageStore final : public storage::PageStore {
public:
    StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) override {
        return inner_.CreateAt(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() override {
        return inner_.CreateNew();
    }
    StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) override {
        return inner_.Get(page_id);
    }
    Status Sync() override {
        return fail_ ? Status::IoError("scripted sync failure") : Status::OK();
    }

    void FailSync(bool fail) noexcept { fail_ = fail; }

private:
    storage::InMemoryPageStore inner_;
    bool fail_ = false;
};

class SuperBlockAnchorTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());

        auto wal = wal::WalManager::Open(device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto page = store_.CreateAt(kSuperBlockPageId);
        ASSERT_TRUE(page.ok());
        superblock_ = SuperBlock::CreateFresh(1000);
        superblock_.Encode(page.value());
    }

    // What a restart sees: the bytes on the store, decoded from scratch.
    StatusOr<SuperBlock> Reload() {
        auto page = store_.Get(kSuperBlockPageId);
        if (!page.ok()) return page.status();
        return SuperBlock::Decode(std::span<const std::byte, kPageSize>(page.value()));
    }

    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> device_;
    std::unique_ptr<wal::WalManager> wal_;
    storage::InMemoryPageStore store_;
    SuperBlock superblock_;
    NoTxns txns_;
};

TEST_F(SuperBlockAnchorTest, APublishedAnchorIsInThePageBytes) {
    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    const wal::CheckpointAnchorRecord record{/*core_id=*/0, /*checkpoint_lsn=*/4096,
                                             /*redo_start_lsn=*/8192, /*durable_lsn=*/12288,
                                             /*segment_no=*/0};
    ASSERT_TRUE(anchor.Publish(record).ok());
    EXPECT_EQ(anchor.publishes(), 1u);

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 8192u);
    EXPECT_EQ(reloaded.value().wal_anchor(0).checkpoint_lsn, 4096u);
    EXPECT_EQ(reloaded.value().wal_anchor(0).durable_lsn, 12288u);
}

TEST_F(SuperBlockAnchorTest, ACheckpointsRedoStartSurvivesARestart) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, 8);
    pool.SetWalDurability(wal_.get());
    storage::BufferPoolCheckpointTarget target(pool);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    // A logged page mutation, in the engine's order: append, then mutate
    // under the record's LSN.
    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
    auto lsn = wal_->Append({wal::RecordType::kHeapInsert, 5, 1, 0});
    ASSERT_TRUE(lsn.ok());
    frame.value()->MarkDirty(lsn.value());
    pool.Unpin(*frame.value());

    wal::Checkpointer checkpointer(*wal_, target, txns_, anchor);
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    // Exactly what the checkpointer computed, read back through the page.
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, checkpointer.redo_start_lsn());
    EXPECT_EQ(reloaded.value().wal_anchor(0).checkpoint_lsn,
              checkpointer.last_checkpoint_lsn());
    EXPECT_EQ(reloaded.value().wal_anchor(0).durable_lsn, wal_->durable_lsn());
    EXPECT_EQ(reloaded.value().wal_anchor(0).segment_no,
              checkpointer.redo_start_lsn() / kSegmentSize);
    EXPECT_EQ(reloaded.value().wal_anchor_count(), 1u);
}

TEST_F(SuperBlockAnchorTest, ASecondCheckpointAdvancesTheAnchorOnDisk) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, 8);
    pool.SetWalDurability(wal_.get());
    storage::BufferPoolCheckpointTarget target(pool);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    wal::Checkpointer checkpointer(*wal_, target, txns_, anchor);

    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    auto first = Reload();
    ASSERT_TRUE(first.ok());
    const std::uint64_t first_redo_start = first.value().wal_anchor(0).redo_start_lsn;

    ASSERT_TRUE(wal_->Append({wal::RecordType::kHeapInsert, 6, 1, 0}).ok());
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());

    auto second = Reload();
    ASSERT_TRUE(second.ok());
    // The whole product of checkpointing: recovery has less to replay than
    // it did before.
    EXPECT_GT(second.value().wal_anchor(0).redo_start_lsn, first_redo_start);
    EXPECT_EQ(anchor.publishes(), 2u);
}

TEST_F(SuperBlockAnchorTest, StreamsFromDifferentCoresDoNotOverwriteEachOther) {
    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 100, 200, 300, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 400, 500, 600, 0}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 200u);
    EXPECT_EQ(reloaded.value().wal_anchor(3).redo_start_lsn, 500u);
    EXPECT_EQ(reloaded.value().wal_anchor_count(), 4u);
}

TEST_F(SuperBlockAnchorTest, AFailedSyncIsReportedAndLeavesTheOldAnchorOnDisk) {
    UnsyncablePageStore store;
    auto page = store.CreateAt(kSuperBlockPageId);
    ASSERT_TRUE(page.ok());
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    sb.Encode(page.value());

    SuperBlockCheckpointAnchor anchor(sb, store);
    ASSERT_TRUE(anchor.Publish({0, 100, 200, 300, 0}).ok());

    store.FailSync(true);
    Status s = anchor.Publish({0, 400, 500, 600, 0});
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kIoError);
    EXPECT_EQ(anchor.publishes(), 1u);  // the failed one does not count

    // Recovery replaying from the *older* redo start costs time, never
    // correctness - which is why a failed publish is safe to report and
    // retry rather than something the caller must repair.
    auto reloaded = store.Get(kSuperBlockPageId);
    ASSERT_TRUE(reloaded.ok());
    auto decoded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(reloaded.value()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_LE(decoded.value().wal_anchor(0).redo_start_lsn, 500u);
}

TEST_F(SuperBlockAnchorTest, PublishingForACoreBeyondTheTableIsRefused) {
    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    Status s = anchor.Publish({kMaxWalCores, 100, 200, 300, 0});
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(anchor.publishes(), 0u);
}

}  // namespace
}  // namespace kds::server
