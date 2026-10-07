#include "kds/wal/stream.hpp"

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_scanner.hpp"
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
        for (std::uint64_t seg = 0; seg < device_->end_segment(); ++seg) {
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
//
// **The volume and the two sizes are chosen to cross both bounds, and
// that is the point of them.** 4 x 500 x ~2 KiB is about 4 MiB against
// 1 MiB segments, so the stream rolls several times; and the staging
// buffer is deliberately the 64 KiB minimum rather than the 1 MiB default,
// because a ring as large as a segment can never fill - the roll drains it
// first, and `OutOfSpace` would be unreachable. Sized under either bound
// this cell would prove only that the encode is serialised, never that a
// concurrent flush or a concurrent segment roll is safe, which is the
// harder half of the seam. The two `EXPECT_GT`s after the join are what
// keep that true if the sizes are ever changed.
TEST_F(SharedStreamTest, EveryThreadsRecordLandsAtTheLsnItWasGiven) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity, /*shared=*/true);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    WalStream& stream = *opened.value();
    ASSERT_TRUE(stream.shared());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    constexpr std::size_t kBigPayload = 2000;
    std::vector<std::vector<Lsn>> issued(kThreads);
    std::atomic<int> drains{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto payload = Pattern(kBigPayload, static_cast<std::uint8_t>(t));
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
                    drains.fetch_add(1, std::memory_order_relaxed);
                    ASSERT_TRUE(stream.Flush().ok());
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    ASSERT_TRUE(stream.Sync().ok());

    // The two paths this volume exists to reach. Without them the cell
    // below would pass on a stream that never flushed or rolled under
    // contention, which is the case that matters.
    EXPECT_GT(drains.load(), 0) << "the ring never filled; the concurrent flush went untested";
    EXPECT_GT(device_->end_segment(), 1u)
        << "the stream never rolled; the concurrent segment roll went untested";

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
    // A flag, not a watermark: 0 is both "never retreated" and a value a
    // retreat could produce, so a sentinel here would read a retreat to
    // zero as a pass.
    std::atomic<bool> went_backwards{false};
    std::thread watcher([&] {
        Lsn last = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const Lsn now = stream.durable_lsn();
            if (now < last) went_backwards.store(true, std::memory_order_relaxed);
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

    EXPECT_FALSE(went_backwards.load()) << "durable_lsn went backwards";
    EXPECT_EQ(stream.durable_lsn(), stream.append_lsn());
}

// ---- The attached manager ------------------------------------------------

TEST_F(SharedStreamTest, AttachRefusesAnUnsharedStream) {
    auto opened = WalStream::Open(device_.get(), 0, kMinRingCapacity);
    ASSERT_TRUE(opened.ok());
    WalWriter writer(device_.get(), [stream = opened.value().get()] { return stream->SyncDevice(); });

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

// **BC-R4 at the stream**: a detach changes the file device's table, and the
// stream's latch is what keeps it from shifting under an unlocked
// `WriteAt` - whose index into the table is relative to the first live
// segment. One thread appends through rolls, another detaches behind it; every
// record must read back in order, from the first live segment to the end.
// Without the latch a record lands in the next segment's file, and the scan
// below stops at the gap it leaves.
TEST(SharedStreamRecyclingTest, ADetachBesideAppendsUnderTheLatchLosesNoRecord) {
    const std::string dir = (std::filesystem::temp_directory_path() /
                             ("kds_shared_stream_recycle_" + std::to_string(::getpid())))
                                .string();
    std::filesystem::remove_all(dir);
    constexpr std::uint64_t kSegment = 64 * 1024;
    auto device = FileLogDevice::Open(dir, 0, kSegment);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto opened = WalStream::Open(device.value().get(), 0, kMinRingCapacity, /*shared=*/true);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    WalStream& stream = *opened.value();

    constexpr int kRecords = 6000;
    std::atomic<bool> done{false};
    std::thread appender([&] {
        for (int i = 0; i < kRecords; ++i) {
            const std::vector<std::byte> payload(kPayloadSize, static_cast<std::byte>(i & 0xFF));
            auto lsn = stream.Append({RecordType::kHeapInsert, 1, static_cast<PageId>(i)}, payload);
            if (!lsn.ok()) {
                ADD_FAILURE() << lsn.status().message();
                break;
            }
            if (i % 16 == 0 && !stream.Flush().ok()) break;
        }
        done.store(true);
    });
    std::uint64_t detached_to = 0;
    while (!done.load()) {
        // Two segments behind the append point: never the one being written.
        const Lsn behind = stream.append_lsn() > 2 * kSegment ? stream.append_lsn() - 2 * kSegment : 0;
        // EXPECT and break, not ASSERT: a return here would leave the
        // appender joinable, and its destructor would end the run.
        const Status detached = stream.DetachBelow(behind);
        EXPECT_TRUE(detached.ok()) << detached.message();
        if (!detached.ok()) break;
        detached_to = device.value()->first_segment();
    }
    appender.join();
    ASSERT_TRUE(stream.Sync().ok());
    ASSERT_GT(detached_to, 0u) << "the cell must have detached while appending";

    // Every record from the first live segment on, in order, each where its
    // page id says it was appended.
    const Lsn from = device.value()->first_segment() * kSegment + kSegmentHeaderSize;
    int seen = 0;
    PageId last = kInvalidPageId;
    auto scanned = ScanLog(*device.value(), 0, from, [&](const DecodedRecord& r) {
        if (last != kInvalidPageId && r.header.page_id != last + 1) {
            return Status::Corruption("record for " + std::to_string(r.header.page_id) +
                                      " follows " + std::to_string(last));
        }
        if (r.payload.empty() || r.payload[0] != static_cast<std::byte>(r.header.page_id & 0xFF)) {
            return Status::Corruption("payload of " + std::to_string(r.header.page_id));
        }
        last = r.header.page_id;
        ++seen;
        return Status::OK();
    });
    ASSERT_TRUE(scanned.ok()) << scanned.status().message();
    EXPECT_FALSE(scanned.value().stopped_early);
    EXPECT_EQ(last, static_cast<PageId>(kRecords - 1)) << "the scan stopped after " << seen;
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace kds::wal
