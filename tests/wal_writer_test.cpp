#include "kds/wal/writer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "kds/wal/memory_log_device.hpp"

// The WAL writer thread (wal/writer.hpp). What it owes, and what the rest of
// the engine leans on:
//
//   - a request returns without doing I/O (the reactor must not block)
//   - the watermark only ever moves forwards, and never past what was asked
//   - a waiter is woken, and a failed sync tells it rather than hanging it
//   - stopping is clean, from any state
//
// The device is a MemoryLogDevice, so "sync" costs nothing - which is the
// point: these are the *protocol*'s tests, not the device's. The latency
// claim this class exists for is measured elsewhere
// (bench/results-latency-matrix.md).

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegment = 1 << 20;

std::unique_ptr<LogDevice> MakeDevice() {
    auto device = MemoryLogDevice::Create(kSegment);
    EXPECT_TRUE(device.ok());
    return std::move(device.value());
}

TEST(WalWriter, StartsAtZeroAndSyncsOnRequest) {
    auto device = MakeDevice();
    WalWriter writer(device.get());
    EXPECT_EQ(writer.durable_lsn(), 0u);

    writer.RequestSync(64);
    ASSERT_TRUE(writer.EnsureDurable(64).ok());
    EXPECT_GE(writer.durable_lsn(), 64u);
    EXPECT_TRUE(writer.IsDurable(64));
}

// The watermark may never name a byte the sync might not have covered: a
// request for 64 makes 64 durable, and says nothing about 128.
TEST(WalWriter, NeverPublishesPastWhatWasAskedFor) {
    auto device = MakeDevice();
    WalWriter writer(device.get());

    ASSERT_TRUE(writer.EnsureDurable(64).ok());
    EXPECT_EQ(writer.durable_lsn(), 64u);
    EXPECT_FALSE(writer.IsDurable(128));
}

// A request behind the watermark is absorbed, not a rewind: the whole engine
// reads this number as "everything below here is safe".
TEST(WalWriter, TheWatermarkOnlyMovesForwards) {
    auto device = MakeDevice();
    WalWriter writer(device.get());

    ASSERT_TRUE(writer.EnsureDurable(4096).ok());
    const Lsn high = writer.durable_lsn();
    writer.RequestSync(8);
    ASSERT_TRUE(writer.EnsureDurable(8).ok());
    EXPECT_EQ(writer.durable_lsn(), high);
}

// An already-durable LSN costs nothing: EnsureDurable is the blocking call,
// so the case that must not block is the one that is already satisfied.
TEST(WalWriter, EnsureDurableReturnsImmediatelyWhenAlreadyPast) {
    auto device = MakeDevice();
    WalWriter writer(device.get());
    ASSERT_TRUE(writer.EnsureDurable(1024).ok());

    const std::uint64_t before = writer.syncs();
    ASSERT_TRUE(writer.EnsureDurable(512).ok());
    EXPECT_EQ(writer.syncs(), before) << "a satisfied wait must not ask for a sync";
}

// Several waiters, one sync: the batching the whole design is for. Every
// waiter below the watermark wakes from the same device call.
TEST(WalWriter, OneSyncSatisfiesEveryWaiterBelowIt) {
    auto device = MakeDevice();
    WalWriter writer(device.get());

    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (int i = 1; i <= 8; ++i) {
        waiters.emplace_back([&writer, &woken, i] {
            if (writer.EnsureDurable(static_cast<Lsn>(i) * 8).ok()) ++woken;
        });
    }
    for (std::thread& t : waiters) t.join();
    EXPECT_EQ(woken.load(), 8);
    EXPECT_GE(writer.durable_lsn(), 64u);
}

// Stopping with nothing outstanding, and stopping twice, are both fine - the
// destructor calls Stop() and a server may have called it already.
TEST(WalWriter, StopIsIdempotent) {
    auto device = MakeDevice();
    WalWriter writer(device.get());
    writer.Stop();
    writer.Stop();
    EXPECT_EQ(writer.failures(), 0u);
}

// **The writer trusts the target it is given**, and that is a contract
// rather than an oversight: it syncs a device and has no way to know which
// bytes were written, so it publishes what was asked for. The guard is one
// layer up - `WalManager::EnsureDurable` refuses an LSN at or past the
// append point, because a page claiming an LSN the log never issued is a
// corrupt page rather than a slow one.
TEST(WalWriter, PublishesTheTargetItWasGiven) {
    auto device = MakeDevice();
    WalWriter writer(device.get());

    // Nothing was ever written here. The writer says so anyway.
    ASSERT_TRUE(writer.EnsureDurable(1 << 20).ok());
    EXPECT_EQ(writer.durable_lsn(), std::uint64_t{1} << 20);
}

// A device whose sync fails, which is the case a waiter must be *told*
// about rather than left hanging for a watermark that is not coming.
class FailingSyncDevice final : public LogDevice {
public:
    std::uint64_t segment_size() const noexcept override { return kSegment; }
    std::uint64_t segment_count() const noexcept override { return 1; }
    Status CreateSegment(std::uint64_t) override { return Status::OK(); }
    Status WriteAt(std::uint64_t, std::uint64_t, std::span<const std::byte>) override {
        return Status::OK();
    }
    Status ReadAt(std::uint64_t, std::uint64_t, std::span<std::byte>) override {
        return Status::OK();
    }
    Status Sync() override { return Status::IoError("device is on fire"); }
};

TEST(WalWriter, AFailedSyncIsReportedAndLeavesTheWatermark) {
    FailingSyncDevice device;
    WalWriter writer(&device);

    Status seen = writer.EnsureDurable(64);
    EXPECT_FALSE(seen.ok());
    EXPECT_EQ(seen.code(), StatusCode::kIoError);

    // The whole point: a sync that failed proves nothing about what reached
    // the platter, so the watermark must not move.
    EXPECT_EQ(writer.durable_lsn(), 0u);
    EXPECT_GE(writer.failures(), 1u);
    EXPECT_EQ(writer.syncs(), 0u);
    EXPECT_FALSE(writer.last_failure().ok());
}

}  // namespace
}  // namespace kds::wal
