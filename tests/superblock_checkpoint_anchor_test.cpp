#include "kds/server/superblock_checkpoint_anchor.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/remote_checkpoint_anchor.hpp"
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
    std::vector<wal::CheckpointActiveTxn> Snapshot() const override { return {}; }
};

// Fails Sync() on demand, so the "the anchor did not land" path is a real
// path and not a comment.
class UnsyncablePageStore final : public storage::PageStore {
public:
    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        return inner_.GetUnpinned(page_id);
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

        auto page = store_.CreateAtUnpinned(kSuperBlockPageId);
        ASSERT_TRUE(page.ok());
        superblock_ = SuperBlock::CreateFresh(1000);
        superblock_.Encode(page.value());
    }

    // What a restart sees: the bytes on the store, decoded from scratch.
    StatusOr<SuperBlock> Reload() {
        auto page = store_.GetUnpinned(kSuperBlockPageId);
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
    sb.Encode(page.value().bytes());

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
    auto decoded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(reloaded.value().bytes()));
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

// ---- The cross-core path (workplan-crosscore.md M5, P2) ---------------
//
// The superblock is page 0 and belongs to the system core, so a checkpoint
// completing anywhere else sends its anchor rather than writing it. What
// must hold is that the two routes produce the **same page**: the remote one
// is a delivery mechanism, not a second implementation.

TEST_F(SuperBlockAnchorTest, AnAnchorSentFromAPeerLandsInThatPeersSlot) {
    auto transport = sched::RealRingTransport::Create(/*core_count=*/3, 16, 128);
    ASSERT_TRUE(transport.ok());

    // Core 2's side: a scheduler to run the send task on, and the anchor
    // that queues it.
    sched::NullIoBackend peer_io;
    sched::Scheduler peer(clock_, peer_io);
    RemoteCheckpointAnchor remote(transport.value(), peer, /*core_id=*/2);

    ASSERT_TRUE(remote.Publish({/*core_id=*/2, 111, 222, 333, 44}).ok());
    EXPECT_EQ(remote.sends(), 1u);
    // Queued, not sent - Publish() returns before the task has run, which is
    // the whole of "fire and forget".
    peer.RunOnce();

    // Core 0's side: the handler Expeditor installs, doing what a local
    // publish does.
    SuperBlockCheckpointAnchor local(superblock_, store_);
    sched::MessageHeader header{};
    std::vector<std::byte> payload;
    ASSERT_TRUE(transport.value().TryReceive(/*dst_core=*/0, header, payload));
    ASSERT_EQ(header.kind, static_cast<std::uint16_t>(sched::RingMessageKind::kAnchorWrite));
    ASSERT_EQ(payload.size(), sizeof(AnchorWritePayload));

    AnchorWritePayload fields{};
    std::memcpy(&fields, payload.data(), sizeof(fields));
    ASSERT_TRUE(local.Publish({fields.core_id, fields.checkpoint_lsn, fields.redo_start_lsn,
                                fields.durable_lsn, fields.segment_no})
                    .ok());

    // In slot 2, not slot 0: the anchor names a WAL stream, and the sender
    // says which - the transport's src_core is a different fact.
    auto reloaded = store_.GetUnpinned(kSuperBlockPageId);
    ASSERT_TRUE(reloaded.ok());
    auto decoded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(reloaded.value()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().wal_anchor(2).checkpoint_lsn, 111u);
    EXPECT_EQ(decoded.value().wal_anchor(2).redo_start_lsn, 222u);
    EXPECT_EQ(decoded.value().wal_anchor(2).durable_lsn, 333u);
    EXPECT_EQ(decoded.value().wal_anchor(2).segment_no, 44u);
    EXPECT_EQ(decoded.value().wal_anchor(0).redo_start_lsn, 0u) << "it landed in the wrong slot";
}

TEST_F(SuperBlockAnchorTest, ThePeersAnchorScheduleSurvivesAMomentarilyFullRing) {
    // Silent drop is forbidden (sched.md §5) even for a message whose loss
    // would be survivable, so the send goes through the retry task.
    auto transport = sched::RealRingTransport::Create(2, /*capacity_slots=*/1, 128);
    ASSERT_TRUE(transport.ok());

    // Fill core 1 -> core 0 so the anchor cannot go out on its first try.
    sched::MessageHeader filler{};
    filler.src_core = 1;
    filler.dst_core = 0;
    filler.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kStepEof);
    ASSERT_TRUE(transport.value().TrySend(filler, {}).ok());

    sched::NullIoBackend peer_io;
    sched::Scheduler peer(clock_, peer_io);
    RemoteCheckpointAnchor remote(transport.value(), peer, /*core_id=*/1);
    ASSERT_TRUE(remote.Publish({1, 10, 20, 30, 0}).ok());

    for (int i = 0; i < 4; ++i) peer.RunOnce();

    // Drain the filler; the anchor goes out on the next iteration rather
    // than having been dropped.
    sched::MessageHeader got{};
    std::vector<std::byte> payload;
    ASSERT_TRUE(transport.value().TryReceive(0, got, payload));
    EXPECT_EQ(got.kind, static_cast<std::uint16_t>(sched::RingMessageKind::kStepEof));

    peer.RunOnce();
    ASSERT_TRUE(transport.value().TryReceive(0, got, payload))
        << "the anchor was dropped when the ring was full";
    EXPECT_EQ(got.kind, static_cast<std::uint16_t>(sched::RingMessageKind::kAnchorWrite));
}

}  // namespace
}  // namespace kds::server
