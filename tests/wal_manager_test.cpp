#include "kds/wal/manager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/record.hpp"

// The manager's job is *when*, not *where* (the stream owns where). So
// these tests are about the durability classes: what is on the platter
// when a commit returns, what one sync resolves, and what a crash takes.
// wal.md section 16-6 (classes), 16-7 (group commit), 16-8 (backpressure).

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 65536;
constexpr std::size_t kPayloadSize = 1000;
constexpr sched::MonoTimeNs kFlushInterval = 10'000'000;  // 10 ms

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 7u) & 0xFF);
    }
    return bytes;
}

RecordSpec HeapInsert(std::uint64_t txn_id, PageId page_id) {
    return RecordSpec{RecordType::kHeapInsert, txn_id, page_id, 0};
}

class WalManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(created.ok()) << created.status().message();
        device_ = std::move(created.value());
    }

    std::unique_ptr<WalManager> OpenManager(WalManagerConfig config = DefaultConfig()) {
        auto opened = WalManager::Open(device_.get(), clock_, 0, config);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        return opened.ok() ? std::move(opened.value()) : nullptr;
    }

    static WalManagerConfig DefaultConfig() {
        WalManagerConfig config;
        config.ring_capacity = kMinRingCapacity;
        config.relaxed_flush_interval_ns = kFlushInterval;
        return config;
    }

    // Every record the device actually holds, in order - the only witness
    // to what a crash would have left behind.
    std::vector<RecordHeaderFields> DurableRecords() {
        std::vector<RecordHeaderFields> found;
        for (std::uint64_t seg = 0; seg < device_->segment_count(); ++seg) {
            std::vector<std::byte> body(kSegmentSize - kSegmentHeaderSize);
            EXPECT_TRUE(device_->ReadAt(seg, kSegmentHeaderSize, body).ok());
            RecordReader reader(body, seg * kSegmentSize + kSegmentHeaderSize);
            while (std::optional<DecodedRecord> record = reader.Next()) {
                if (record->type() == RecordType::kPad) {
                    break;
                }
                found.push_back(record->header);
            }
        }
        return found;
    }

    bool DurableHasCommitFor(std::uint64_t txn_id) {
        for (const auto& header : DurableRecords()) {
            if (header.type == static_cast<std::uint8_t>(RecordType::kTxnCommit) &&
                header.txn_id == txn_id) {
                return true;
            }
        }
        return false;
    }

    sched::ManualClock clock_;
    std::unique_ptr<MemoryLogDevice> device_;
};

// ---- D1 strict -----------------------------------------------------------

TEST_F(WalManagerTest, StrictCommitIsDurableBeforeItReturns) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    ASSERT_TRUE(wal->Append(HeapInsert(1, 7), Pattern(kPayloadSize, 1)).ok());

    auto commit = wal->Commit(1, DurabilityClass::kStrict);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    // The whole point of D1: the ack is safe the instant Commit() returns.
    EXPECT_TRUE(wal->IsDurable(commit.value()));
    EXPECT_TRUE(DurableHasCommitFor(1));
    EXPECT_EQ(wal->stats().strict_commits, 1u);
    EXPECT_EQ(wal->stats().syncs, 1u);
}

TEST_F(WalManagerTest, StrictCommitReportsAFailedSyncAndStaysUndurable) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);

    device_->FailNextSync(Status::IoError("injected"));
    auto commit = wal->Commit(1, DurabilityClass::kStrict);
    ASSERT_FALSE(commit.ok());
    EXPECT_EQ(commit.status().code(), StatusCode::kIoError);
    // A commit that could not be made durable must not be acknowledged,
    // and nothing here may claim it was.
    EXPECT_EQ(wal->stats().strict_commits, 0u);
    EXPECT_EQ(wal->stats().sync_failures, 1u);
}

// ---- D2 group ------------------------------------------------------------

TEST_F(WalManagerTest, GroupCommitStagesUntilTheDrain) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);

    auto commit = wal->Commit(1, DurabilityClass::kGroup);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_FALSE(wal->IsDurable(commit.value()));
    EXPECT_TRUE(wal->HasPendingGroupCommits());
    EXPECT_EQ(wal->stats().syncs, 0u);

    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_TRUE(wal->IsDurable(commit.value()));
    EXPECT_FALSE(wal->HasPendingGroupCommits());
}

TEST_F(WalManagerTest, ManyCommittersResolveOnOneSync) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);

    // wal.md section 16-7: N concurrent committers, one flush, and every
    // one of them resumes with durable_lsn >= its commit LSN.
    constexpr int kCommitters = 16;
    std::vector<Lsn> commit_lsns;
    for (int i = 0; i < kCommitters; ++i) {
        ASSERT_TRUE(wal->Append(HeapInsert(i + 1, 1), Pattern(64, 2)).ok());
        auto commit = wal->Commit(i + 1, DurabilityClass::kGroup);
        ASSERT_TRUE(commit.ok()) << commit.status().message();
        commit_lsns.push_back(commit.value());
    }
    EXPECT_EQ(wal->stats().syncs, 0u);

    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_EQ(wal->stats().syncs, 1u);
    EXPECT_EQ(wal->stats().group_batches, 1u);
    EXPECT_EQ(wal->stats().mean_group_batch_size(), kCommitters);

    for (Lsn lsn : commit_lsns) {
        EXPECT_TRUE(wal->IsDurable(lsn));
    }
}

TEST_F(WalManagerTest, AFailedBatchSyncLeavesEveryCommitterWaiting) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    auto first = wal->Commit(1, DurabilityClass::kGroup);
    auto second = wal->Commit(2, DurabilityClass::kGroup);
    ASSERT_TRUE(first.ok() && second.ok());

    device_->FailNextSync(Status::IoError("injected"));
    EXPECT_EQ(wal->DrainOnce().code(), StatusCode::kIoError);
    // The batch is not resolved and must not be forgotten: these two are
    // still unacknowledged, and the next drain still owes them a sync.
    EXPECT_TRUE(wal->HasPendingGroupCommits());
    EXPECT_FALSE(wal->IsDurable(second.value()));
    EXPECT_EQ(wal->stats().group_batches, 0u);

    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_TRUE(wal->IsDurable(second.value()));
    EXPECT_FALSE(wal->HasPendingGroupCommits());
}

TEST_F(WalManagerTest, GroupCommitsDoNotWaitForTheRelaxedInterval) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    auto commit = wal->Commit(1, DurabilityClass::kGroup);
    ASSERT_TRUE(commit.ok());

    // Clock never advances: a D2 commit is a zero-loss class, so the drain
    // that serves it cannot be gated on the D3 timer.
    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_TRUE(wal->IsDurable(commit.value()));
    EXPECT_EQ(wal->stats().interval_syncs, 0u);
}

// ---- D3 relaxed ----------------------------------------------------------

TEST_F(WalManagerTest, RelaxedCommitReturnsBeforeAnySync) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);

    auto commit = wal->Commit(1, DurabilityClass::kRelaxed);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_FALSE(wal->IsDurable(commit.value()));
    // Nothing waits on it, so nothing is pending in the group-commit sense.
    EXPECT_FALSE(wal->HasPendingGroupCommits());
    EXPECT_EQ(wal->stats().relaxed_commits, 1u);
    EXPECT_EQ(wal->stats().syncs, 0u);
}

TEST_F(WalManagerTest, RelaxedLossWindowIsBoundedByTheInterval) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    auto commit = wal->Commit(1, DurabilityClass::kRelaxed);
    ASSERT_TRUE(commit.ok());

    // Just short of the window: still exposed.
    clock_.Advance(kFlushInterval - 1);
    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_FALSE(wal->IsDurable(commit.value()));

    // At the window: synced. This is the bound wal.md section 16-6 wants
    // asserted, not just documented.
    clock_.Advance(1);
    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_TRUE(wal->IsDurable(commit.value()));
    EXPECT_EQ(wal->stats().interval_syncs, 1u);
}

TEST_F(WalManagerTest, ARelaxedCommitIsWhatACrashTakes) {
    auto strict_lsn = Lsn{0};
    {
        auto wal = OpenManager();
        ASSERT_NE(wal, nullptr);
        auto strict = wal->Commit(1, DurabilityClass::kStrict);
        ASSERT_TRUE(strict.ok());
        strict_lsn = strict.value();
        ASSERT_TRUE(wal->Commit(2, DurabilityClass::kRelaxed).ok());
    }
    device_->Crash();

    // The acknowledged-under-D1 commit survived; the relaxed one inside
    // its loss window did not. That asymmetry is the entire product
    // difference between the classes.
    EXPECT_TRUE(DurableHasCommitFor(1));
    EXPECT_FALSE(DurableHasCommitFor(2));

    auto reopened = OpenManager();
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(reopened->IsDurable(strict_lsn));
}

// ---- Idle drains ---------------------------------------------------------

TEST_F(WalManagerTest, DrainingAnIdleManagerCostsNothing) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    const std::uint64_t writes_before = device_->stats().writes;

    // The drain runs on every reactor iteration, so a tick with nothing
    // pending has to be free.
    for (int i = 0; i < 100; ++i) {
        clock_.Advance(kFlushInterval);
        ASSERT_TRUE(wal->DrainOnce().ok());
    }
    EXPECT_EQ(wal->stats().syncs, 0u);
    EXPECT_EQ(device_->stats().writes, writes_before);
}

// ---- The WalDurability seam ---------------------------------------------

TEST_F(WalManagerTest, EnsureDurableSyncsUpToARecordAndIsThenFree) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    auto page_lsn = wal->Append(HeapInsert(1, 42), Pattern(kPayloadSize, 3));
    ASSERT_TRUE(page_lsn.ok());
    ASSERT_TRUE(wal->Append(HeapInsert(1, 43), Pattern(kPayloadSize, 4)).ok());

    // This is what BufferPool::Flush will call before writing page 42.
    EXPECT_FALSE(wal->IsDurable(page_lsn.value()));
    ASSERT_TRUE(wal->EnsureDurable(page_lsn.value()).ok());
    EXPECT_TRUE(wal->IsDurable(page_lsn.value()));
    EXPECT_EQ(wal->stats().syncs, 1u);

    // Already past it: no second sync. The common case on the flush path
    // is that the log ran ahead long ago.
    ASSERT_TRUE(wal->EnsureDurable(page_lsn.value()).ok());
    EXPECT_EQ(wal->stats().syncs, 1u);
}

TEST_F(WalManagerTest, ANeverLoggedPageIsAlwaysFlushable) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    // page_lsn 0 means "never logged" (record.hpp), and WAL-before-data
    // has nothing to say about a page with no log records.
    EXPECT_TRUE(wal->IsDurable(0));
    ASSERT_TRUE(wal->EnsureDurable(0).ok());
    EXPECT_EQ(wal->stats().syncs, 0u);
}

TEST_F(WalManagerTest, EnsureDurableRejectsAnLsnTheLogNeverIssued) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    ASSERT_TRUE(wal->Append(HeapInsert(1, 1), Pattern(64, 5)).ok());

    // A page stamped with a page_lsn past the append point is corrupt, not
    // slow - syncing forever would never make it durable.
    const Status status = wal->EnsureDurable(wal->appended_lsn() + 8);
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(wal->EnsureDurable(wal->appended_lsn()).code(), StatusCode::kInvalidArgument);
}

// ---- Backpressure --------------------------------------------------------

TEST_F(WalManagerTest, AFullRingDrainsInlineInsteadOfFailingTheAppender) {
    auto big_device = MemoryLogDevice::Create(1024 * 1024);
    ASSERT_TRUE(big_device.ok());
    auto opened = WalManager::Open(big_device.value().get(), clock_, 0, DefaultConfig());
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto wal = std::move(opened.value());

    // Well past the ring's capacity: the appender must never see
    // OutOfSpace, because with synchronous I/O the inline flush is the
    // drain it would otherwise suspend for (wal.md section 6-4).
    const std::vector<std::byte> payload = Pattern(kPayloadSize, 6);
    const std::size_t records = (kMinRingCapacity / EncodedRecordSize(payload.size())) * 3;
    for (std::size_t i = 0; i < records; ++i) {
        auto lsn = wal->Append(HeapInsert(1, 1), payload);
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    }
    EXPECT_GE(wal->stats().ring_full_drains, 1u);  // the stall metric
    EXPECT_EQ(wal->stats().records_appended, records);
}

// ---- Abort ---------------------------------------------------------------

TEST_F(WalManagerTest, AbortIsLoggedWithoutASync) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);
    ASSERT_TRUE(wal->Append(HeapInsert(9, 1), Pattern(64, 7)).ok());

    auto abort = wal->Abort(9);
    ASSERT_TRUE(abort.ok()) << abort.status().message();
    // A lost abort record costs nothing: a transaction with no commit
    // record is a loser either way (wal.md section 12-1).
    EXPECT_EQ(wal->stats().syncs, 0u);
    EXPECT_FALSE(wal->HasPendingGroupCommits());
}

// ---- Mixed traffic -------------------------------------------------------

TEST_F(WalManagerTest, OneSyncServesEveryClassStagedBehindIt) {
    auto wal = OpenManager();
    ASSERT_NE(wal, nullptr);

    auto relaxed = wal->Commit(1, DurabilityClass::kRelaxed);
    auto group = wal->Commit(2, DurabilityClass::kGroup);
    ASSERT_TRUE(relaxed.ok() && group.ok());
    // The D2 commit's sync carries the D3 record staged before it - a
    // sync is a watermark, not a per-record operation.
    ASSERT_TRUE(wal->DrainOnce().ok());
    EXPECT_TRUE(wal->IsDurable(relaxed.value()));
    EXPECT_TRUE(wal->IsDurable(group.value()));
    EXPECT_EQ(wal->stats().syncs, 1u);

    // And a strict commit after them syncs only its own tail.
    auto strict = wal->Commit(3, DurabilityClass::kStrict);
    ASSERT_TRUE(strict.ok());
    EXPECT_EQ(wal->stats().syncs, 2u);
    EXPECT_TRUE(DurableHasCommitFor(1));
    EXPECT_TRUE(DurableHasCommitFor(2));
    EXPECT_TRUE(DurableHasCommitFor(3));
}

}  // namespace
}  // namespace kds::wal
