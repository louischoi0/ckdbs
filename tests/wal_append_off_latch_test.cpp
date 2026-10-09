// **The WAL append's I/O moves out from under the stream latch** (BA-S12,
// BA-R9's first half, `instructions/v3.0.0/workorder-ba-parallelism.md`).
//
// BA-S4's census found the stream latch at 10 % of reactor time in a clean
// cell, held across a flush's `pwrite` and a roll's whole segment build -
// a 64 MiB reservation, zeroing and `fsync`. Two things move: the next
// segment's body is built ahead by the device once the current one is half
// full, so a roll writes only a header and a name; and a flush swaps the
// staged ring for a second buffer under the latch and writes it with the
// latch released, so appenders keep staging across the `pwrite`.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/stream.hpp"

namespace kds::wal {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kSegment = 64 * 1024;

std::string TempDir(const char* name) {
    return (std::filesystem::temp_directory_path() /
            (std::string("kds_wal_off_latch_") + name + "_" + std::to_string(::getpid())))
        .string();
}

bool Eventually(const std::function<bool()>& pred) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// The LSNs of every record in the log, in scan order.
std::vector<Lsn> ScanLsns(LogDevice& device) {
    std::vector<Lsn> lsns;
    auto scanned = ScanLog(device, 0, 0, [&](const DecodedRecord& r) {
        lsns.push_back(r.header.lsn);
        return Status::OK();
    });
    EXPECT_TRUE(scanned.ok()) << scanned.status().message();
    return lsns;
}

TEST(WalAppendOffLatchTest, PastHalfASegmentTheNextOneIsBuiltAheadAndTheRollAdoptsIt) {
    const std::string dir = TempDir("prepare");
    std::filesystem::remove_all(dir);
    {
        auto device = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(device.ok()) << device.status().message();
        auto stream = WalStream::Open(device.value().get(), 0, kDefaultRingCapacity, /*shared=*/true);
        ASSERT_TRUE(stream.ok()) << stream.status().message();
        const std::vector<std::byte> payload(512, std::byte{0x5A});
        // Under half a segment: nothing is built ahead.
        while (stream.value()->append_lsn() % kSegment < kSegment / 4) {
            ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
        }
        EXPECT_EQ(device.value()->PreparedSegmentForTest(), std::numeric_limits<std::uint64_t>::max());
        // Past it: segment 1's body is built, under the temporary name.
        while (stream.value()->append_lsn() % kSegment < kSegment * 3 / 4) {
            ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
        }
        ASSERT_TRUE(Eventually([&] { return device.value()->PreparedSegmentForTest() == 1; }))
            << "segment 1 was never built ahead";
        EXPECT_TRUE(std::filesystem::exists(dir + "/wal-0-1.log.tmp"));
        EXPECT_FALSE(std::filesystem::exists(dir + "/wal-0-1.log"));
        // The roll adopts it: the final name appears, the temporary one goes.
        while (device.value()->end_segment() < 2) {
            ASSERT_TRUE(stream.value()->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
        }
        EXPECT_EQ(device.value()->PreparedSegmentForTest(), std::numeric_limits<std::uint64_t>::max());
        EXPECT_TRUE(std::filesystem::exists(dir + "/wal-0-1.log"));
        EXPECT_FALSE(std::filesystem::exists(dir + "/wal-0-1.log.tmp"));
        ASSERT_TRUE(stream.value()->Sync().ok());
    }
    // Reopened: the adopted segment scans, its header the real one.
    auto reopened = FileLogDevice::Open(dir, 0, kSegment);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    const std::vector<Lsn> lsns = ScanLsns(*reopened.value());
    ASSERT_FALSE(lsns.empty());
    EXPECT_GE(lsns.back(), kSegment) << "nothing scanned past the roll into the adopted segment";
    std::filesystem::remove_all(dir);
}

TEST(WalAppendOffLatchTest, ASegmentBuiltAheadAndNeverAdoptedIsNotPartOfTheLog) {
    const std::string dir = TempDir("leftover");
    std::filesystem::remove_all(dir);
    {
        auto device = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(device.ok());
        ASSERT_TRUE(device.value()->CreateSegment(0).ok());
        device.value()->PrepareSegment(1);
        ASSERT_TRUE(Eventually([&] { return device.value()->PreparedSegmentForTest() == 1; }));
        // The process ends with the body built and no roll: a crash.
    }
    auto reopened = FileLogDevice::Open(dir, 0, kSegment);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(reopened.value()->end_segment(), 1u) << "a body built ahead was adopted at open";
    // And the next creation of that number removes the leftover and builds.
    EXPECT_TRUE(reopened.value()->CreateSegment(1).ok());
    std::filesystem::remove_all(dir);
}

// A device whose writes block until released: a flush stopped inside its
// `pwrite`.
class GatedWriteDevice final : public LogDevice {
public:
    explicit GatedWriteDevice(LogDevice& inner) : inner_(inner) {}
    void Hold() {
        std::lock_guard<std::mutex> g(m_);
        held_ = true;
    }
    void Release() {
        {
            std::lock_guard<std::mutex> g(m_);
            held_ = false;
        }
        cv_.notify_all();
    }
    int parked() const { return parked_.load(); }

    std::uint64_t segment_size() const noexcept override { return inner_.segment_size(); }
    std::uint64_t first_segment() const noexcept override { return inner_.first_segment(); }
    std::uint64_t end_segment() const noexcept override { return inner_.end_segment(); }
    Status CreateSegment(std::uint64_t n, std::span<const std::byte> header) override {
        return inner_.CreateSegment(n, header);
    }
    Status WriteAt(std::uint64_t n, std::uint64_t off, std::span<const std::byte> in) override {
        {
            std::unique_lock<std::mutex> g(m_);
            if (held_) {
                ++parked_;
                cv_.wait(g, [this] { return !held_; });
                --parked_;
            }
        }
        return inner_.WriteAt(n, off, in);
    }
    Status ReadAt(std::uint64_t n, std::uint64_t off, std::span<std::byte> out) override {
        return inner_.ReadAt(n, off, out);
    }
    Status Sync() override { return inner_.Sync(); }
    Status DetachBelow(std::uint64_t n) override {
        if (parked_.load() > 0) detached_beside_a_write_.store(true);
        return inner_.DetachBelow(n);
    }
    bool detached_beside_a_write() const { return detached_beside_a_write_.load(); }
    Status ReclaimDetached() override { return inner_.ReclaimDetached(); }
    std::uint64_t segments_removed() const noexcept override { return inner_.segments_removed(); }

private:
    LogDevice& inner_;
    std::mutex m_;
    std::condition_variable cv_;
    bool held_ = false;
    std::atomic<int> parked_{0};
    std::atomic<bool> detached_beside_a_write_{false};
};

TEST(WalAppendOffLatchTest, AnAppendIsNotHeldBehindAFlushsWrite) {
    auto inner = MemoryLogDevice::Create(kSegment);
    ASSERT_TRUE(inner.ok());
    GatedWriteDevice device(*inner.value());
    auto stream = WalStream::Open(&device, 0, kDefaultRingCapacity, /*shared=*/true);
    ASSERT_TRUE(stream.ok()) << stream.status().message();
    WalStream& s = *stream.value();
    const std::vector<std::byte> payload(64, std::byte{0x11});
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());

    device.Hold();
    std::thread flusher([&] { EXPECT_TRUE(s.Flush().ok()); });
    ASSERT_TRUE(Eventually([&] { return device.parked() == 1; })) << "the flush never reached its write";

    // The flush is inside its write. An append completes meanwhile - red
    // before BA-S12, where the write held the stream latch.
    std::atomic<bool> appended{false};
    std::thread appender([&] {
        for (int i = 0; i < 10; ++i) EXPECT_TRUE(s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
        appended.store(true);
    });
    EXPECT_TRUE(Eventually([&] { return appended.load(); }))
        << "an append waited for another thread's flush write";
    device.Release();
    flusher.join();
    appender.join();

    // Everything reaches the log, once each, in LSN order.
    ASSERT_TRUE(s.Sync().ok());
    const std::vector<Lsn> lsns = ScanLsns(*inner.value());
    ASSERT_EQ(lsns.size(), 20u);
    for (std::size_t i = 1; i < lsns.size(); ++i) EXPECT_LT(lsns[i - 1], lsns[i]);
}

TEST(WalAppendOffLatchTest, ARollWaitsOutAFlushInFlightAndTheLogStaysInOrder) {
    auto inner = MemoryLogDevice::Create(kSegment);
    ASSERT_TRUE(inner.ok());
    GatedWriteDevice device(*inner.value());
    auto stream = WalStream::Open(&device, 0, kDefaultRingCapacity, /*shared=*/true);
    ASSERT_TRUE(stream.ok()) << stream.status().message();
    WalStream& s = *stream.value();
    const std::vector<std::byte> payload(1024, std::byte{0x22});
    std::size_t appended = 0;
    while (s.append_lsn() % kSegment < kSegment / 2) {
        ASSERT_TRUE(s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
        ++appended;
    }

    device.Hold();
    std::thread flusher([&] { EXPECT_TRUE(s.Flush().ok()); });
    ASSERT_TRUE(Eventually([&] { return device.parked() == 1; }));
    // Appends that roll into segment 1: the roll's seal flushes under the
    // latch, which waits for the write in flight - so it parks behind the
    // gate too, and nothing is written past the held bytes.
    std::thread roller([&] {
        while (device.end_segment() < 2) {
            if (!s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok()) break;
            ++appended;
        }
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(device.end_segment(), 1u) << "the roll ran ahead of a flush still writing below it";
    device.Release();
    flusher.join();
    roller.join();
    ASSERT_TRUE(s.Sync().ok());
    const std::vector<Lsn> lsns = ScanLsns(*inner.value());
    ASSERT_EQ(lsns.size(), appended);
    for (std::size_t i = 1; i < lsns.size(); ++i) EXPECT_LT(lsns[i - 1], lsns[i]);
}

// A detach changes the device's segment table, which `FileLogDevice::WriteAt`
// reads unlocked on the promise that no table change runs beside it. The
// unlatched flush's write is outside the latch, so the detach has to wait
// it out - red when it only took the latch.
TEST(WalAppendOffLatchTest, ADetachWaitsOutAFlushInFlight) {
    auto inner = MemoryLogDevice::Create(kSegment);
    ASSERT_TRUE(inner.ok());
    GatedWriteDevice device(*inner.value());
    auto stream = WalStream::Open(&device, 0, kDefaultRingCapacity, /*shared=*/true);
    ASSERT_TRUE(stream.ok()) << stream.status().message();
    WalStream& s = *stream.value();
    const std::vector<std::byte> payload(1024, std::byte{0x33});
    while (device.end_segment() < 3) {
        ASSERT_TRUE(s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
    }
    ASSERT_TRUE(s.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());

    device.Hold();
    std::thread flusher([&] { EXPECT_TRUE(s.Flush().ok()); });
    ASSERT_TRUE(Eventually([&] { return device.parked() == 1; }));
    std::thread detacher([&] { EXPECT_TRUE(s.DetachBelow(2 * kSegment).ok()); });
    std::this_thread::sleep_for(50ms);
    device.Release();
    flusher.join();
    detacher.join();
    EXPECT_FALSE(device.detached_beside_a_write()) << "a detach ran beside a flush's write";
    EXPECT_EQ(device.first_segment(), 2u);
}

}  // namespace
}  // namespace kds::wal
