#include "kds/server/superblock_checkpoint_anchor.hpp"

#include <array>
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
    // And nothing folded: per-core streams keep one anchor per stream,
    // which is the proof AL-S3 changed no behaviour a running instance has.
    EXPECT_EQ(anchor.folded_cores(), 0u);
}

// ---- The fold under one stream (AR0 M0, work order AL's AL-R4) ----------

// Turns the fixture's superblock into a single-stream one. Patching the
// page and decoding is the only route in - there is no setter, deliberately
// (`superblock.hpp`) - and it is the route a real single-stream volume
// takes, through the same `Decode`.
class SuperBlockFoldTest : public SuperBlockAnchorTest {
protected:
    // `cores` matters: the fold holds the anchor where the mount found it
    // until every core has published, so a fixture at the 1-core default
    // would never exercise the warm-up.
    void MakeSingleStream(std::uint32_t cores = 4) {
        superblock_ = SuperBlock::CreateFresh(1000, storage::kDefaultInlineCellWidth, cores);
        std::array<std::byte, kPageSize> buf{};
        superblock_.Encode(std::span<std::byte, kPageSize>(buf));
        const std::uint32_t single = kSingleStream;
        std::memcpy(buf.data() + kSuperBlockBodyOffset + kLogTopologyOffset, &single,
                    sizeof(single));
        auto decoded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(buf));
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        superblock_ = std::move(decoded.value());
        ASSERT_TRUE(superblock_.single_stream());

        auto page = store_.GetUnpinned(kSuperBlockPageId);
        ASSERT_TRUE(page.ok());
        superblock_.Encode(page.value());
    }
};

// The stage's headline: two cores checkpoint at different points and the
// one anchor names the lower. Starting at the higher would skip records
// the other core still needs replayed.
TEST_F(SuperBlockFoldTest, TheAnchorNamesTheLowestRedoStartOverEveryCore) {
    MakeSingleStream();
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 950, 850, 1050, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/2, 970, 870, 1070, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 400, 200, 600, 0}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 200u);
    // And no peer slot was touched: one stream, one anchor.
    EXPECT_EQ(reloaded.value().wal_anchor(3).redo_start_lsn, 0u);
    EXPECT_EQ(reloaded.value().wal_anchor_count(), 1u);
}

// The four numbers must stay one core's consistent set. A field-wise
// minimum would describe a checkpoint that never happened.
TEST_F(SuperBlockFoldTest, TheFoldCarriesTheWholeSetFromTheCoreThatSuppliedTheMinimum) {
    MakeSingleStream();
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 7}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/2, 950, 850, 1050, 9}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 970, 870, 1070, 9}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 400, 200, 600, 2}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    const WalAnchorFields got = reloaded.value().wal_anchor(0);
    EXPECT_EQ(got.redo_start_lsn, 200u);
    EXPECT_EQ(got.checkpoint_lsn, 400u);   // core 1's, not core 0's 900
    EXPECT_EQ(got.durable_lsn, 600u);      // core 1's, not core 0's 1000
    EXPECT_EQ(got.segment_no, 2u);         // core 1's, not core 0's 7
}

// A core checkpointing again must be able to lift the floor - otherwise the
// anchor is pinned at the oldest number any core ever published and the
// bounded-RTO guarantee the checkpoint exists for never improves.
TEST_F(SuperBlockFoldTest, ACoresLaterCheckpointReplacesItsOwnContributionToTheFold) {
    MakeSingleStream();
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/2, 950, 850, 1050, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 970, 870, 1070, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 400, 200, 600, 0}).ok());
    ASSERT_EQ(Reload().value().wal_anchor(0).redo_start_lsn, 200u);

    // Core 1 moves past core 0, so core 0 is now the laggard.
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 2000, 1900, 2100, 0}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 800u);
    EXPECT_EQ(anchor.folded_cores(), 4u);
}

// **The case the warm-up exists for.** Core 0 checkpoints first at a high
// redo start while cores 1..3 have not checkpointed in this run at all.
// Folding over the map alone would move the one anchor to core 0's number
// and the next crash would replay from there - past records the other
// cores' still-dirty pages need. The anchor must stay where the mount left
// it until every core has spoken.
TEST_F(SuperBlockFoldTest, TheAnchorDoesNotAdvanceWhileACoreHasNeverPublished) {
    MakeSingleStream(/*cores=*/4);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 950, 850, 1050, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/2, 970, 870, 1070, 0}).ok());

    // Three of four. A fresh database's mount anchor is all zeroes, which
    // reads as "replay from the start of the stream" - safe, and not 800.
    auto held = Reload();
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().wal_anchor(0).redo_start_lsn, 0u);

    // The fourth core speaks and the anchor may move.
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 990, 890, 1090, 0}).ok());
    auto moved = Reload();
    ASSERT_TRUE(moved.ok());
    EXPECT_EQ(moved.value().wal_anchor(0).redo_start_lsn, 800u);
}

// The floor the warm-up holds to is the mount's own anchor, not zero - a
// database that has run before must not be pushed back to replaying its
// whole log every time it restarts.
TEST_F(SuperBlockFoldTest, TheWarmUpHoldsAtTheMountAnchorRatherThanTheStartOfTheLog) {
    MakeSingleStream(/*cores=*/4);
    ASSERT_TRUE(superblock_.SetWalAnchor(0, WalAnchorFields{5000, 4000, 6000, 1}).ok());
    {
        auto page = store_.GetUnpinned(kSuperBlockPageId);
        ASSERT_TRUE(page.ok());
        superblock_.Encode(page.value());
    }

    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 9000, 8000, 9500, 2}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 4000u)
        << "the warm-up must hold the mount's anchor, not reset to zero";
}

// A sync failure leaves the map ahead of the page - a state the fold
// created and the per-core path never had. The map is the source of truth
// and is monotone per core, so the next successful publish rewrites the
// whole fold including the contribution whose sync failed.
TEST_F(SuperBlockFoldTest, AFailedSyncKeepsTheCoresContributionForTheNextPublish) {
    UnsyncablePageStore store;
    auto page = store.CreateAtUnpinned(kSuperBlockPageId);
    ASSERT_TRUE(page.ok());
    superblock_ = SuperBlock::CreateFresh(1000, storage::kDefaultInlineCellWidth, /*cores=*/4);
    {
        std::array<std::byte, kPageSize> buf{};
        superblock_.Encode(std::span<std::byte, kPageSize>(buf));
        const std::uint32_t single = kSingleStream;
        std::memcpy(buf.data() + kSuperBlockBodyOffset + kLogTopologyOffset, &single,
                    sizeof(single));
        auto decoded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(buf));
        ASSERT_TRUE(decoded.ok());
        superblock_ = std::move(decoded.value());
    }
    superblock_.Encode(page.value());

    SuperBlockCheckpointAnchor anchor(superblock_, store);
    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 950, 850, 1050, 0}).ok());
    ASSERT_TRUE(anchor.Publish({/*core_id=*/2, 970, 870, 1070, 0}).ok());

    // Core 3's publish reaches the map but not the platter.
    store.FailSync(true);
    EXPECT_FALSE(anchor.Publish({/*core_id=*/3, 500, 400, 600, 0}).ok());
    EXPECT_EQ(anchor.folded_cores(), 4u);

    // It republishes higher; the fold must still know 400 was superseded
    // and land on core 0's 800 as the new minimum.
    store.FailSync(false);
    ASSERT_TRUE(anchor.Publish({/*core_id=*/3, 2000, 1900, 2100, 0}).ok());

    auto on_disk = store.GetUnpinned(kSuperBlockPageId);
    ASSERT_TRUE(on_disk.ok());
    auto reloaded = SuperBlock::Decode(std::span<const std::byte, kPageSize>(on_disk.value()));
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 800u);
}

// The warm-up gate asks *which* cores have published, not how many
// anchors arrived. An id the volume has no core for would otherwise set a
// bit `core_count` does not account for, release the warm-up early, and
// put a phantom core's number into the minimum.
TEST_F(SuperBlockFoldTest, AnAnchorNamingACoreTheVolumeDoesNotHaveIsRefused) {
    MakeSingleStream(/*cores=*/4);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    Status refused = anchor.Publish({/*core_id=*/9, 400, 200, 600, 0});
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(anchor.folded_cores(), 0u);
    EXPECT_EQ(anchor.publishes(), 0u);
}

// The floor must ignore slots nobody ever published into. Under one
// stream slots 1..63 hold zeros forever, and a minimum that counted them
// would be 0 on every mount - pinning the warm-up at replay-the-whole-log
// and writing that zero over the real anchor. This is the cell that caught
// it when the floor was first widened to every slot.
TEST_F(SuperBlockFoldTest, TheFloorIgnoresSlotsNobodyEverPublishedInto) {
    MakeSingleStream(/*cores=*/4);
    ASSERT_TRUE(superblock_.SetWalAnchor(0, WalAnchorFields{5000, 4000, 6000, 1}).ok());
    {
        auto page = store_.GetUnpinned(kSuperBlockPageId);
        ASSERT_TRUE(page.ok());
        superblock_.Encode(page.value());
    }
    // Slots 1..63 are all zero here, as they are on every single-stream
    // volume, and the floor must still be core 0's 4000.
    SuperBlockCheckpointAnchor anchor(superblock_, store_);
    ASSERT_TRUE(anchor.Publish({/*core_id=*/1, 9000, 8000, 9500, 2}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 4000u);
}

// One core is the degenerate fold, and it must be an identity - this is the
// shape a single-core instance and, later, M1's gathered checkpoint take.
TEST_F(SuperBlockFoldTest, OneCoreFoldsToItself) {
    MakeSingleStream(/*cores=*/1);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    ASSERT_TRUE(anchor.Publish({/*core_id=*/0, 900, 800, 1000, 4}).ok());

    auto reloaded = Reload();
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value().wal_anchor(0).redo_start_lsn, 800u);
    EXPECT_EQ(reloaded.value().wal_anchor(0).segment_no, 4u);
}

// The refusal AL-S2 added is unreachable through this path, because the
// fold redirects every core to slot 0 before it is consulted. Without the
// fold, a peer's publish would hard-fail the moment the topology flipped.
TEST_F(SuperBlockFoldTest, APeersPublishSucceedsRatherThanHittingTheSlotRefusal) {
    MakeSingleStream(/*cores=*/8);
    SuperBlockCheckpointAnchor anchor(superblock_, store_);

    Status s = anchor.Publish({/*core_id=*/5, 400, 200, 600, 0});
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(anchor.publishes(), 1u);
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
