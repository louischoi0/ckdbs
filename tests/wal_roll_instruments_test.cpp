#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"
#include "kds/base/log.hpp"
#include "kds/sched/clock.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/stream.hpp"
#include "sim/instance.hpp"
#include "two_core_rig.hpp"

// BQ-S1 (`instructions/v3.0.0/workorder-bq-wal-segment-roll.md`): the
// segment roll's instruments. A roll's hold of the stream latch is timed
// step by step (`RollRecord`, `RollStat`), the drain's waits on that latch
// are counted apart, and so is the part of any stream-latch wait a roll's
// hold covered. Each cell forces one shape of roll and reads what it moved:
// a roll behind a held sync with a drain blocked behind it, rolls at one
// core with their log lines, a body built inline, a body waited for, and
// `SHOW META` on an owning and on an attached core.

namespace kds::wal {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kSegment = kMinSegmentSize;

std::string TempDir(const char* name) {
    return (std::filesystem::temp_directory_path() /
            (std::string("kds_wal_roll_instr_") + name + "_" + std::to_string(::getpid())))
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

// The roll window's clock (`RollRecord::start_ns`).
std::uint64_t SteadyNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t SlotTally(std::uint32_t core, Tally t) {
    return detail::g_contention[core % detail::kContentionSlots]
        .tallies[static_cast<std::size_t>(t)]
        .load(std::memory_order_relaxed);
}

// The integer after ` <name>=` in a reply or a line; -1 when absent.
std::int64_t Field(const std::string& text, const std::string& name) {
    const std::string key = " " + name + "=";
    const auto at = text.find(key);
    if (at == std::string::npos) return -1;
    return std::stoll(text.substr(at + key.size()));
}

// A `FileLogDevice` whose sync can be held - the writer thread parks inside
// it holding the stream's sync gate, so a roll that arrives waits it out
// with the stream latch held - and whose build ahead can be switched off.
// Forwards the timed calls, so the split the cells read is the real
// device's.
class HeldSyncDevice final : public LogDevice {
public:
    explicit HeldSyncDevice(FileLogDevice& inner) : inner_(inner) {}

    void Hold() {
        std::lock_guard<std::mutex> g(mu_);
        held_ = true;
    }
    void Release() {
        {
            std::lock_guard<std::mutex> g(mu_);
            held_ = false;
        }
        cv_.notify_all();
    }
    int parked() const { return parked_.load(); }
    void SkipPrepare() { skip_prepare_ = true; }

    std::uint64_t segment_size() const noexcept override { return inner_.segment_size(); }
    std::uint64_t first_segment() const noexcept override { return inner_.first_segment(); }
    std::uint64_t end_segment() const noexcept override { return inner_.end_segment(); }
    Status CreateSegment(std::uint64_t n, std::span<const std::byte> h) override {
        return inner_.CreateSegment(n, h);
    }
    Status CreateSegmentTimed(std::uint64_t n, std::span<const std::byte> h,
                              SegmentCreateTiming& out) override {
        return inner_.CreateSegmentTimed(n, h, out);
    }
    void PrepareSegment(std::uint64_t n) override {
        if (!skip_prepare_) inner_.PrepareSegment(n);
    }
    Status WriteAt(std::uint64_t n, std::uint64_t off, std::span<const std::byte> in) override {
        return inner_.WriteAt(n, off, in);
    }
    Status ReadAt(std::uint64_t n, std::uint64_t off, std::span<std::byte> out) override {
        return inner_.ReadAt(n, off, out);
    }
    Status Sync() override {
        DeviceSyncTiming discarded;
        return SyncTimed(discarded);
    }
    Status SyncTimed(DeviceSyncTiming& out) override {
        {
            std::unique_lock<std::mutex> g(mu_);
            if (held_) {
                parked_.fetch_add(1);
                cv_.wait(g, [this] { return !held_; });
            }
        }
        return inner_.SyncTimed(out);
    }
    Status DetachBelow(std::uint64_t n) override { return inner_.DetachBelow(n); }
    Status ReclaimDetached() override { return inner_.ReclaimDetached(); }
    std::uint64_t segments_removed() const noexcept override { return inner_.segments_removed(); }

private:
    FileLogDevice& inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool held_ = false;
    std::atomic<int> parked_{0};
    std::atomic<bool> skip_prepare_{false};
};

// Appends 512-byte records until the device has `min_segments` and the
// stream sits `slack` bytes or fewer from its segment's end.
void FillTo(WalStream& stream, std::uint64_t min_segments, LogDevice& device,
            std::uint64_t slack) {
    const std::vector<std::byte> payload(512, std::byte{0x5A});
    while (device.end_segment() < min_segments ||
           kSegment - stream.append_lsn() % kSegment > slack) {
        ASSERT_TRUE(stream.Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
    }
}

// A record that cannot fit the slack `FillTo` left: the append rolls.
StatusOr<Lsn> AppendRolling(WalStream& stream, RollRecord* roll) {
    const std::vector<std::byte> payload(8192, std::byte{0x33});
    return stream.Append({RecordType::kHeapInsert, 1, 7, 0}, payload, roll);
}

// The row's two-core cell: a roll on core 0 whose device sync waits out the
// writer's held one, with core 1's drain blocked on the stream latch behind
// it. The gate's wait runs to the release and carries the hold, the
// waited-out sync is reported, the steps fit inside the hold, and core 1's
// slot counts both its drain wait and the part of it the roll covered.
TEST(WalRollInstrumentsTest, ARollBehindAHeldSyncIsTimedAndChargesTheDrainItBlocked) {
    const std::string dir = TempDir("held");
    std::filesystem::remove_all(dir);
    {
        auto file = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(file.ok()) << file.status().message();
        HeldSyncDevice device(*file.value());
        sched::ManualClock clock;
        WalManagerConfig config;
        config.shared_stream = true;
        auto owner = WalManager::Open(&device, clock, /*core_id=*/0, config);
        ASSERT_TRUE(owner.ok()) << owner.status().message();
        owner.value()->StartWriter();
        auto peer = WalManager::Attach(owner.value()->stream(), owner.value()->writer(), clock,
                                       /*core_id=*/1);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        WalStream& stream = *owner.value()->stream();
        ASSERT_TRUE(stream.shared());

        // Destroyed before the managers: a failed assertion inside the held
        // window releases the writer and joins the threads rather than
        // hanging in the writer's stop.
        std::thread roller;
        std::thread drainer;
        struct Unwind {
            HeldSyncDevice& device;
            std::thread& roller;
            std::thread& drainer;
            ~Unwind() {
                device.Release();
                if (roller.joinable()) roller.join();
                if (drainer.joinable()) drainer.join();
            }
        } unwind{device, roller, drainer};

        {
            CurrentCoreGuard core0(0);
            // Two live segments, so the roll's sync has a tail and a rest,
            // and the next body asked for.
            FillTo(stream, 2, device, 4096);
        }
        ASSERT_TRUE(Eventually([&] { return file.value()->PreparedSegmentForTest() == 2; }));

        // The writer parks inside a held sync, holding the gate.
        device.Hold();
        {
            CurrentCoreGuard core1(1);
            ASSERT_TRUE(peer.value()->Commit(11, DurabilityClass::kGroup).ok());
            ASSERT_TRUE(peer.value()->DrainOnce().ok());
            // A second commit, so the drain below has a batch to ask for.
            ASSERT_TRUE(peer.value()->Commit(12, DurabilityClass::kGroup).ok());
        }
        ASSERT_TRUE(Eventually([&] { return device.parked() >= 1; }));

        const std::uint64_t rolls_before = stream.roll_stat(RollStat::kRolls);
        const std::uint64_t flush_before = stream.roll_stat(RollStat::kFlushNs);
        const std::uint64_t drain_before = SlotTally(1, Tally::kWalDrainLatchWaitNs);
        const std::uint64_t blocked_before = SlotTally(1, Tally::kWalRollBlockedWaits);
        const std::uint64_t blocked_ns_before = SlotTally(1, Tally::kWalRollBlockedWaitNs);

        RollRecord roll;
        roller = std::thread([&] {
            CurrentCoreGuard core0(0);
            EXPECT_TRUE(AppendRolling(stream, &roll).ok());
        });
        // The roll is past its seal and on its way to the gate, the latch held.
        ASSERT_TRUE(Eventually([&] { return stream.sealed(); }));
        std::atomic<bool> entered{false};
        drainer = std::thread([&] {
            CurrentCoreGuard core1(1);
            entered.store(true, std::memory_order_release);
            EXPECT_TRUE(peer.value()->DrainOnce().ok());
        });
        ASSERT_TRUE(Eventually([&] { return entered.load(std::memory_order_acquire); }));
        std::this_thread::sleep_for(100ms);
        const std::uint64_t released_at = SteadyNs();
        device.Release();
        roller.join();
        drainer.join();

        EXPECT_EQ(stream.roll_stat(RollStat::kRolls), rolls_before + 1);
        ASSERT_NE(roll.end_ns, 0u) << "the append did not report its roll";
        EXPECT_EQ(roll.segment_no, 2u);
        EXPECT_EQ(roll.core, 0u);
        EXPECT_GE(roll.hold_ns(), 80'000'000u);
        // The gate is held until the release, so the wait reaches it; the
        // arrival is at most the seal and the flush past the roll's start.
        EXPECT_GT(roll.sync_gate_ns, 0u);
        EXPECT_GE(roll.start_ns + roll.seal_ns + roll.flush_ns + roll.sync_gate_ns + 1'000'000u,
                  released_at);
        // The steps are disjoint parts of the hold.
        EXPECT_LE(roll.seal_ns + roll.flush_ns + roll.sync_gate_ns + roll.sync_ns + roll.create_ns,
                  roll.hold_ns());
        EXPECT_GT(roll.seal_ns, 0u);
        EXPECT_GT(roll.flush_ns, 0u);
        EXPECT_GT(stream.roll_stat(RollStat::kFlushNs), flush_before);
        EXPECT_GT(roll.sync_ns, 0u);
        EXPECT_EQ(roll.sync.segments, 2u);
        EXPECT_GT(roll.sync.tail_ns, 0u);
        EXPECT_GT(roll.sync.rest_ns, 0u);
        EXPECT_GE(roll.waited_syncs, 1u) << "the held sync finished while the roll waited";
        EXPECT_GE(roll.waited_sync.segments, 2u);
        EXPECT_GT(roll.create_ns, 0u);
        EXPECT_EQ(roll.create.body, BodyOutcome::kHit);
        EXPECT_GT(roll.create.header_ns, 0u);
        EXPECT_GT(roll.create.rename_ns, 0u);
        EXPECT_GT(roll.create.dir_sync_ns, 0u);
        EXPECT_GE(stream.roll_stat(RollStat::kHoldMaxNs), roll.hold_ns());
        EXPECT_GE(stream.roll_stat(RollStat::kBodyHit), 1u);
        EXPECT_GE(stream.roll_stat(RollStat::kSyncSegmentsMax), 2u);

        // Core 1's drain waited on the latch the roll held, and the roll's
        // hold covered that wait - never more of it than the wait was.
        const std::uint64_t drain_ns = SlotTally(1, Tally::kWalDrainLatchWaitNs) - drain_before;
        const std::uint64_t blocked_ns =
            SlotTally(1, Tally::kWalRollBlockedWaitNs) - blocked_ns_before;
        EXPECT_GE(drain_ns, 50'000'000u);
        EXPECT_GE(SlotTally(1, Tally::kWalRollBlockedWaits) - blocked_before, 1u);
        EXPECT_GE(blocked_ns, 50'000'000u);
        EXPECT_LE(blocked_ns, drain_ns + 1'000'000u);
    }
    std::filesystem::remove_all(dir);
}

// At one core the stream is unshared and its latch null: two rolls move the
// step timers and the totals, each adopting a body built ahead, the first
// syncing one segment and the second two. The gate is never contended - no
// writer thread - and the drain's latch wait and the roll-blocked tallies
// stay exactly 0. Each roll writes one Info line whose window lies inside
// the cell's and agrees with its hold.
TEST(WalRollInstrumentsTest, AtOneCoreRollsAreTimedAndLoggedAndNoLatchWaitIsCharged) {
    const std::string dir = TempDir("one");
    std::filesystem::remove_all(dir);
    {
        auto file = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(file.ok()) << file.status().message();
        sched::ManualClock clock;
        auto manager = WalManager::Open(file.value().get(), clock, /*core_id=*/0);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        MemoryLogSink sink;
        ManualWallClock wall;
        Logger log(&sink, wall, LogLevel::kInfo);
        manager.value()->SetLogger(&log);
        WalStream& stream = *manager.value()->stream();
        ASSERT_FALSE(stream.shared());

        CurrentCoreGuard core0(0);
        const std::uint64_t drain_before = SlotTally(0, Tally::kWalDrainLatchWaitNs);
        const std::uint64_t blocked_before = SlotTally(0, Tally::kWalRollBlockedWaits);
        const std::uint64_t started = SteadyNs();
        const std::vector<std::byte> payload(512, std::byte{0x5A});
        bool checked_first = false;
        while (file.value()->end_segment() < 3) {
            ASSERT_TRUE(manager.value()->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
            if (!checked_first && stream.roll_stat(RollStat::kRolls) == 1) {
                checked_first = true;
                EXPECT_GT(stream.roll_stat(RollStat::kSyncTailNs), 0u);
                EXPECT_EQ(stream.roll_stat(RollStat::kSyncRestNs), 0u)
                    << "the first roll's sync covered one segment: it has no rest";
            }
            if (stream.append_lsn() % kSegment > kSegment * 3 / 4) {
                // Past half a segment: let the body built ahead finish, so
                // the roll adopts it.
                const std::uint64_t next = stream.append_lsn() / kSegment + 1;
                ASSERT_TRUE(Eventually(
                    [&] { return file.value()->PreparedSegmentForTest() == next; }));
            }
            clock.Advance(20'000'000);
            ASSERT_TRUE(manager.value()->DrainOnce().ok());
        }
        const std::uint64_t finished = SteadyNs();
        ASSERT_TRUE(checked_first);

        EXPECT_EQ(stream.roll_stat(RollStat::kRolls), 2u);
        EXPECT_EQ(stream.roll_stat(RollStat::kSyncGateNs), 0u)
            << "one core, no writer thread: the sync gate is never contended";
        EXPECT_GT(stream.roll_stat(RollStat::kFlushNs), 0u);
        EXPECT_GT(stream.roll_stat(RollStat::kSyncRestNs), 0u) << "the second roll had a rest";
        EXPECT_EQ(stream.roll_stat(RollStat::kSyncSegments), 3u);
        EXPECT_EQ(stream.roll_stat(RollStat::kSyncSegmentsMax), 2u);
        EXPECT_LE(stream.roll_stat(RollStat::kSealNs) + stream.roll_stat(RollStat::kFlushNs) +
                      stream.roll_stat(RollStat::kSyncGateNs) + stream.roll_stat(RollStat::kSyncNs) +
                      stream.roll_stat(RollStat::kCreateNs),
                  stream.roll_stat(RollStat::kHoldNs));
        EXPECT_GT(stream.roll_stat(RollStat::kHeaderNs), 0u);
        EXPECT_GT(stream.roll_stat(RollStat::kRenameNs), 0u);
        EXPECT_GT(stream.roll_stat(RollStat::kDirSyncNs), 0u);
        EXPECT_EQ(stream.roll_stat(RollStat::kBodyHit), 2u);
        EXPECT_EQ(stream.roll_stat(RollStat::kBodyInline), 0u);
        EXPECT_EQ(SlotTally(0, Tally::kWalDrainLatchWaitNs), drain_before);
        EXPECT_EQ(SlotTally(0, Tally::kWalRollBlockedWaits), blocked_before);

        int roll_lines = 0;
        for (const std::string& line : sink.lines) {
            if (line.find("[wal] roll seg=") == std::string::npos) continue;
            ++roll_lines;
            const std::int64_t start = Field(line, "start_ns");
            const std::int64_t end = Field(line, "end_ns");
            EXPECT_LE(static_cast<std::int64_t>(started), start) << line;
            EXPECT_LE(start, end) << line;
            EXPECT_LE(end, static_cast<std::int64_t>(finished)) << line;
            EXPECT_EQ(Field(line, "hold_us"), (end - start) / 1000) << line;
            EXPECT_NE(line.find(" body=hit "), std::string::npos) << line;
        }
        EXPECT_EQ(roll_lines, 2);
    }
    std::filesystem::remove_all(dir);
}

// With no body asked for, the roll builds it inline, and says so.
TEST(WalRollInstrumentsTest, ARollWithNoBodyBuiltAheadBuildsItInline) {
    const std::string dir = TempDir("inline");
    std::filesystem::remove_all(dir);
    {
        auto file = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(file.ok()) << file.status().message();
        HeldSyncDevice device(*file.value());
        device.SkipPrepare();
        auto stream = WalStream::Open(&device, 0);
        ASSERT_TRUE(stream.ok()) << stream.status().message();
        FillTo(*stream.value(), 1, device, 4096);
        RollRecord roll;
        ASSERT_TRUE(AppendRolling(*stream.value(), &roll).ok());
        EXPECT_EQ(roll.create.body, BodyOutcome::kInline);
        EXPECT_GT(roll.create.body_ns, 0u);
        EXPECT_EQ(roll.create.body_wait_ns, 0u);
        EXPECT_EQ(stream.value()->roll_stat(RollStat::kBodyInline), 1u);
        EXPECT_EQ(stream.value()->roll_stat(RollStat::kBodyHit), 0u);
        EXPECT_GT(stream.value()->roll_stat(RollStat::kBodyNs), 0u);
    }
    std::filesystem::remove_all(dir);
}

// A body built ahead that is not yet ready: the roll waits for the build,
// adopts it, and counts the wait.
TEST(WalRollInstrumentsTest, ARollThatMeetsABuildInProgressWaitsForItAndSaysSo) {
    const std::string dir = TempDir("waited");
    std::filesystem::remove_all(dir);
    {
        auto file = FileLogDevice::Open(dir, 0, kSegment);
        ASSERT_TRUE(file.ok()) << file.status().message();
        auto stream = WalStream::Open(file.value().get(), 0);
        ASSERT_TRUE(stream.ok()) << stream.status().message();
        FillTo(*stream.value(), 1, *file.value(), 4096);
        ASSERT_TRUE(Eventually([&] { return file.value()->PreparedSegmentForTest() == 1; }));

        std::unique_lock<std::mutex> building = file.value()->HoldPrepareForTest();
        RollRecord roll;
        std::thread roller([&] { EXPECT_TRUE(AppendRolling(*stream.value(), &roll).ok()); });
        std::this_thread::sleep_for(50ms);
        building.unlock();
        roller.join();

        EXPECT_EQ(roll.create.body, BodyOutcome::kWaited);
        EXPECT_GE(roll.create.body_wait_ns, 40'000'000u);
        EXPECT_EQ(roll.create.body_ns, 0u) << "the body was adopted, not built";
        EXPECT_EQ(stream.value()->roll_stat(RollStat::kBodyWaited), 1u);
        EXPECT_GE(stream.value()->roll_stat(RollStat::kBodyWaitNs), 40'000'000u);
    }
    std::filesystem::remove_all(dir);
}

// `SHOW META` prints the roll totals, the stream's byte position and the
// three contention tallies, in microseconds where the name says so, and
// the totals move once statements roll.
TEST(WalRollInstrumentsTest, ShowMetaPrintsTheRollTotals) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    ASSERT_EQ(db.Execute("CREATE TABLE r (id int64, v char(200))").rfind("ERR", 0),
              std::string::npos);
    // One value per row: the key is issued.
    std::string values;
    for (int i = 0; i < 100; ++i) values += std::string(i == 0 ? "" : ", ") + "('x')";
    const std::uint64_t started = SteadyNs();
    for (int i = 0; i < 40; ++i) {
        ASSERT_EQ(db.Execute("INSERT INTO r VALUES " + values).rfind("ERR", 0), std::string::npos);
    }
    const std::uint64_t elapsed_us = (SteadyNs() - started) / 1000;

    const std::string meta = db.Execute("SHOW META");
    for (const char* name : kRollStatNames) {
        EXPECT_NE(meta.find(std::string(" ") + name + "="), std::string::npos) << name;
    }
    EXPECT_NE(meta.find(" wal_append_lsn="), std::string::npos) << meta;
    for (const char* name :
         {"wal_drain_latch_wait_us", "wal_roll_blocked_waits", "wal_roll_blocked_wait_us"}) {
        EXPECT_NE(meta.find(std::string(" contention_") + name + "="), std::string::npos) << name;
    }
    EXPECT_GE(Field(meta, "wal_rolls"), 1) << "40 inserts of 100 rows never rolled";
    // Microseconds, not nanoseconds: inside the time the inserts took.
    const std::int64_t hold_us = Field(meta, "wal_roll_hold_us");
    const std::int64_t hold_max_us = Field(meta, "wal_roll_hold_max_us");
    EXPECT_LE(hold_max_us, hold_us);
    EXPECT_LE(hold_us, static_cast<std::int64_t>(elapsed_us));
}

// The roll totals are the stream's, so an attached core prints them too,
// and the same values as the owning core.
TEST(WalRollInstrumentsTest, AnAttachedCorePrintsTheSameRollTotals) {
    auto opened = server::TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<server::TwoCoreRig> rig = std::move(opened.value());
    const std::string peer = rig->core(1).dispatcher().Dispatch("SHOW META").response;
    const std::string owner = rig->core(0).dispatcher().Dispatch("SHOW META").response;
    for (const char* name : kRollStatNames) {
        EXPECT_NE(peer.find(std::string(" ") + name + "="), std::string::npos) << name;
    }
    ASSERT_GE(Field(peer, "wal_append_lsn"), 0) << peer;
    EXPECT_EQ(Field(peer, "wal_append_lsn"), Field(owner, "wal_append_lsn"));
    EXPECT_EQ(Field(peer, "wal_rolls"), Field(owner, "wal_rolls"));
}

}  // namespace
}  // namespace kds::wal
