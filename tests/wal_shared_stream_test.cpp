#include "kds/wal/stream.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/writer.hpp"

// AL-S1a (`instructions/v3.0.0/workorder-al-m0-single-wal.md`): the one
// stream several cores append to, and the attached manager that syncs
// through the instance's writer instead of its own device.
//
// What these cells are about is the *seam*, not the durability classes -
// `wal_manager_test.cpp` owns those, and every one of its cases still runs
// against an unshared stream, which is the `cores = 1` path.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 1 << 20;
constexpr std::size_t kPayloadSize = 200;

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 31u) & 0xFF);
    }
    return bytes;
}

RecordSpec HeapInsert(std::uint64_t txn_id, PageId page_id) {
    return RecordSpec{RecordType::kHeapInsert, txn_id, page_id, 0};
}

class SharedStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(created.ok()) << created.status().message();
        device_ = std::move(created.value());
    }

    // Every record on the device, in LSN order, across every segment.
    std::vector<RecordHeaderFields> RecordsOnDevice() {
        std::vector<RecordHeaderFields> found;
        for (std::uint64_t seg = 0; seg < device_->segment_count(); ++seg) {
            std::vector<std::byte> body(kSegmentSize - kSegmentHeaderSize);
            EXPECT_TRUE(device_->ReadAt(seg, kSegmentHeaderSize, body).ok());
            RecordReader reader(body, seg * kSegmentSize + kSegmentHeaderSize);
            while (std::optional<DecodedRecord> record = reader.Next()) {
                if (record->type() == RecordType::kPad) break;
                found.push_back(record->header);
            }
        }
        return found;
    }

    sched::ManualClock clock_;
    std::unique_ptr<MemoryLogDevice> device_;
};

// ---- The stream ----------------------------------------------------------

TEST_F(SharedStreamTest, AnUnsharedStreamTakesNoLatchAndIsTheDefault) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    EXPECT_FALSE(opened.value()->shared());
}

// The seam's core property: N threads append concurrently, every record
// lands exactly once, at the LSN its appender was handed, and the bytes
// scan back in one unbroken LSN-ordered run.
TEST_F(SharedStreamTest, EveryThreadsRecordLandsAtTheLsnItWasGiven) {
    auto opened = WalStream::Open(device_.get(), 0, kDefaultRingCapacity, /*shared=*/true);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    WalStream& stream = *opened.value();
    ASSERT_TRUE(stream.shared());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::vector<Lsn>> issued(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto payload = Pattern(kPayloadSize, static_cast<std::uint8_t>(t));
                for (;;) {
                    auto lsn = stream.Append(HeapInsert(static_cast<std::uint64_t>(t) + 1,
                                                        static_cast<PageId>(i)),
                                             payload);
                    if (lsn.ok()) {
                        issued[t].push_back(lsn.value());
                        break;
                    }
                    // The ring filled: drain and retry, which is what the
                    // reactor's appender does (wal.md §6-4).
                    ASSERT_EQ(lsn.status().code(), StatusCode::kOutOfSpace)
                        << lsn.status().message();
                    ASSERT_TRUE(stream.Flush().ok());
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    ASSERT_TRUE(stream.Sync().ok());

    std::multiset<Lsn> handed_out;
    for (const auto& per_thread : issued) {
        ASSERT_EQ(per_thread.size(), static_cast<std::size_t>(kPerThread));
        handed_out.insert(per_thread.begin(), per_thread.end());
    }

    const std::vector<RecordHeaderFields> on_device = RecordsOnDevice();
    ASSERT_EQ(on_device.size(), handed_out.size());

    // No LSN was issued twice, the records are in LSN order with no gap,
    // and each one sits at an LSN some appender was told about.
    std::set<Lsn> unique(handed_out.begin(), handed_out.end());
    EXPECT_EQ(unique.size(), handed_out.size()) << "an LSN was handed to two appenders";
    Lsn previous = 0;
    for (const RecordHeaderFields& header : on_device) {
        EXPECT_EQ(unique.count(header.lsn), 1u) << "record at an LSN nobody was given";
        EXPECT_GT(header.lsn, previous);
        previous = header.lsn;
    }
}

// A record every appender can see the effect of: the watermark only ever
// moves forwards, whichever thread syncs.
TEST_F(SharedStreamTest, TheDurableWatermarkNeverMovesBackwardsUnderConcurrentSyncs) {
    auto opened = WalStream::Open(device_.get(), 0, kDefaultRingCapacity, /*shared=*/true);
    ASSERT_TRUE(opened.ok());
    WalStream& stream = *opened.value();

    std::atomic<bool> stop{false};
    std::atomic<Lsn> lowest_seen{0};
    std::thread watcher([&] {
        Lsn last = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const Lsn now = stream.durable_lsn();
            if (now < last) lowest_seen.store(now, std::memory_order_relaxed);
            last = now;
        }
    });

    std::vector<std::thread> syncers;
    for (int t = 0; t < 3; ++t) {
        syncers.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) {
                ASSERT_TRUE(
                    stream.Append(HeapInsert(static_cast<std::uint64_t>(t) + 1, 1), {}).ok());
                ASSERT_TRUE(stream.Sync().ok());
            }
        });
    }
    for (std::thread& thread : syncers) thread.join();
    stop.store(true, std::memory_order_relaxed);
    watcher.join();

    EXPECT_EQ(lowest_seen.load(), 0u) << "durable_lsn went backwards";
    EXPECT_EQ(stream.durable_lsn(), stream.append_lsn());
}

// ---- The attached manager ------------------------------------------------

TEST_F(SharedStreamTest, AttachRefusesAnUnsharedStream) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity);
    ASSERT_TRUE(opened.ok());
    WalWriter writer(device_.get());

    auto attached = WalManager::Attach(opened.value().get(), &writer, clock_, 1);
    EXPECT_FALSE(attached.ok());
    EXPECT_EQ(attached.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(SharedStreamTest, AttachRefusesANullWriter) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity, /*shared=*/true);
    ASSERT_TRUE(opened.ok());

    auto attached = WalManager::Attach(opened.value().get(), nullptr, clock_, 1);
    EXPECT_FALSE(attached.ok());
    EXPECT_EQ(attached.status().code(), StatusCode::kInvalidArgument);
}

// The peer's commit becomes durable, and the peer performed no device sync
// of its own doing so - the whole point of M0 for a peer core (AL-2).
TEST_F(SharedStreamTest, AnAttachedManagersCommitIsMadeDurableByTheWriter) {
    WalManagerConfig owner_config;
    owner_config.ring_capacity = kDefaultRingCapacity;
    owner_config.shared_stream = true;
    auto owner = WalManager::Open(device_.get(), clock_, /*core_id=*/0, owner_config);
    ASSERT_TRUE(owner.ok()) << owner.status().message();
    owner.value()->StartWriter();

    auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock_,
                                   /*core_id=*/1);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    EXPECT_TRUE(peer.value()->attached());
    EXPECT_EQ(peer.value()->core_id(), 1u);
    // The stream is the instance's, so its number stays 0 whoever appends.
    EXPECT_EQ(peer.value()->stream()->core_id(), 0u);

    ASSERT_TRUE(peer.value()->Append(HeapInsert(7, 3), Pattern(kPayloadSize, 2)).ok());
    auto commit = peer.value()->Commit(7, DurabilityClass::kStrict);
    ASSERT_TRUE(commit.ok()) << commit.status().message();

    // **D1's contract, on a peer**: durable when Commit returns, with no
    // further wait by the caller (AL-S1b).
    EXPECT_TRUE(peer.value()->IsDurable(commit.value()));

    // The peer never synced the device itself, and the writer's count is
    // the owner's to report - a peer's reads 0 so that N cores cannot
    // report the same shared number N times.
    EXPECT_EQ(peer.value()->stats().syncs, 0u);
    EXPECT_EQ(peer.value()->writer_syncs(), 0u);
    EXPECT_GT(owner.value()->writer_syncs(), 0u);
}

// The WAL-before-data gate (`wal.md` §8-1): the page store calls this
// before writing a dirty page, so an OK that means "asked for" rather than
// "on the platter" would let a data page overtake its log record. On a
// peer it must block on the writer.
TEST_F(SharedStreamTest, AnAttachedManagersEnsureDurableBlocksUntilThePlatter) {
    WalManagerConfig owner_config;
    owner_config.ring_capacity = kDefaultRingCapacity;
    owner_config.shared_stream = true;
    auto owner = WalManager::Open(device_.get(), clock_, /*core_id=*/0, owner_config);
    ASSERT_TRUE(owner.ok());
    owner.value()->StartWriter();

    auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock_,
                                   /*core_id=*/1);
    ASSERT_TRUE(peer.ok());

    auto lsn = peer.value()->Append(HeapInsert(11, 5), Pattern(kPayloadSize, 3));
    ASSERT_TRUE(lsn.ok());
    ASSERT_FALSE(peer.value()->IsDurable(lsn.value() + 1));

    ASSERT_TRUE(peer.value()->EnsureDurable(lsn.value()).ok());
    // The gate returned, so the record it named is on the platter now -
    // not merely requested.
    EXPECT_TRUE(peer.value()->IsDurable(lsn.value()));
    EXPECT_EQ(peer.value()->stats().syncs, 0u);
}

// The other promise that may not return early: everything acknowledged
// has landed. A clean shutdown and a client's SYNC both rest on it.
TEST_F(SharedStreamTest, AnAttachedManagersSyncAllLeavesNothingUnwritten) {
    WalManagerConfig owner_config;
    owner_config.ring_capacity = kDefaultRingCapacity;
    owner_config.shared_stream = true;
    auto owner = WalManager::Open(device_.get(), clock_, /*core_id=*/0, owner_config);
    ASSERT_TRUE(owner.ok());
    owner.value()->StartWriter();

    auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock_,
                                   /*core_id=*/1);
    ASSERT_TRUE(peer.ok());

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(peer.value()
                        ->Append(HeapInsert(12, static_cast<PageId>(i)), Pattern(kPayloadSize, 4))
                        .ok());
    }
    ASSERT_TRUE(peer.value()->Commit(12, DurabilityClass::kRelaxed).ok());

    ASSERT_TRUE(peer.value()->SyncAll().ok());
    EXPECT_EQ(peer.value()->durable_lsn(), peer.value()->appended_lsn());
}

// The drain is the one path that must NOT wait: it runs on the reactor
// once a tick, and blocking it would hold every other session on the core
// for another thread's fdatasync. It asks, and a later tick closes the
// batch.
TEST_F(SharedStreamTest, AnAttachedManagersDrainAsksAndDoesNotWait) {
    WalManagerConfig owner_config;
    owner_config.ring_capacity = kDefaultRingCapacity;
    owner_config.shared_stream = true;
    auto owner = WalManager::Open(device_.get(), clock_, /*core_id=*/0, owner_config);
    ASSERT_TRUE(owner.ok());
    owner.value()->StartWriter();

    auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock_,
                                   /*core_id=*/1);
    ASSERT_TRUE(peer.ok());

    auto commit = peer.value()->Commit(13, DurabilityClass::kGroup);
    ASSERT_TRUE(commit.ok());
    ASSERT_TRUE(peer.value()->DrainOnce().ok());

    // Whether the writer has finished by now is a race this test does not
    // depend on; what it pins is that the drain performed no device sync
    // of its own and that the batch does close once the platter catches up.
    EXPECT_EQ(peer.value()->stats().syncs, 0u);
    ASSERT_TRUE(owner.value()->writer()->EnsureDurable(commit.value() + 1).ok());
    ASSERT_TRUE(peer.value()->DrainOnce().ok());
    EXPECT_FALSE(peer.value()->HasPendingGroupCommits());
    EXPECT_EQ(peer.value()->stats().group_batches, 1u);
}

// The batch bookkeeping is per core, and a batch made durable by somebody
// else's sync still closes on this core's next drain.
TEST_F(SharedStreamTest, APeersGroupBatchClosesOnTheDrainAfterAnotherCoresSync) {
    WalManagerConfig owner_config;
    owner_config.ring_capacity = kDefaultRingCapacity;
    owner_config.shared_stream = true;
    auto owner = WalManager::Open(device_.get(), clock_, /*core_id=*/0, owner_config);
    ASSERT_TRUE(owner.ok());
    owner.value()->StartWriter();

    auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock_,
                                   /*core_id=*/1);
    ASSERT_TRUE(peer.ok());

    auto commit = peer.value()->Commit(9, DurabilityClass::kGroup);
    ASSERT_TRUE(commit.ok());
    EXPECT_TRUE(peer.value()->HasPendingGroupCommits());
    EXPECT_EQ(peer.value()->stats().group_batches, 0u);

    // Core 0 syncs for its own reasons; the peer's record rides along.
    ASSERT_TRUE(owner.value()->SyncAll().ok());
    ASSERT_TRUE(peer.value()->IsDurable(commit.value()));

    ASSERT_TRUE(peer.value()->DrainOnce().ok());
    EXPECT_FALSE(peer.value()->HasPendingGroupCommits());
    EXPECT_EQ(peer.value()->stats().group_batches, 1u);
    EXPECT_EQ(peer.value()->stats().syncs, 0u) << "the peer synced for a batch it did not own";
}

}  // namespace
}  // namespace kds::wal
