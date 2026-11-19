// **A read that would wait leaves its reactor** (DE,
// `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md`): the
// pieces, each on one thread.
//
// - The device's no-wait read and its simulator twin
//   (`PageDevice::ReadPageNoWait`, `MemoryPageDevice::NoWait`).
// - The store's refusing arm on its miss path (`storage/miss_refusal.hpp`).
// - The page reader (`storage/page_reader.hpp`).
// - A top-level `SELECT`'s head discarded and run again, through
//   `CommandDispatcher::Dispatch` over a simulator instance whose reader has
//   no thread (DE-Q12) - the rules DE-R1 (the answer does not change),
//   DE-R2 and DE-Q10 (i)-(ii) (nothing but a top-level `SELECT`'s body is
//   refused; a discarded run records nothing), DE-R7 (the run after the
//   last refused one reads synchronously) and DE-R9 (one `io.page_read` a
//   read).
//
// The park itself - a real thread, a wake, a deadline - is
// `de_page_read_rig_test.cpp`'s.

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "frame_budget_override.hpp"
#include "kds/base/contention.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/miss_refusal.hpp"
#include "kds/storage/page_reader.hpp"
#include "sim/instance.hpp"

namespace kds::storage {
namespace {

using NoWait = MemoryPageDevice::NoWait;

// ---- The device -------------------------------------------------------------

TEST(DePageDeviceTest, TheMemoryDeviceAnswersWouldWaitUntilAPageIsWarmedAndOnceAfter) {
    auto made = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_TRUE(made.ok()) << made.status().message();
    MemoryPageDevice& device = *made.value();
    std::array<std::byte, kPageSize> in{};
    in[17] = std::byte{0x5a};
    ASSERT_TRUE(device.WritePage(3, in).ok());

    std::array<std::byte, kPageSize> out{};
    bool would_wait = true;
    // The default mode is the base class's: a read, never a wait.
    ASSERT_TRUE(device.ReadPageNoWait(3, out, &would_wait).ok());
    EXPECT_FALSE(would_wait);
    EXPECT_EQ(out[17], std::byte{0x5a});

    device.SetNoWait(NoWait::kUntilWarmed);
    const std::uint64_t reads_before = device.stats().reads;
    ASSERT_TRUE(device.ReadPageNoWait(3, out, &would_wait).ok());
    EXPECT_TRUE(would_wait);
    EXPECT_EQ(device.stats().reads, reads_before) << "a would-wait answer transferred nothing";
    EXPECT_EQ(device.would_wait_answers(), 1u);

    device.WarmPage(3);
    out.fill(std::byte{0});
    ASSERT_TRUE(device.ReadPageNoWait(3, out, &would_wait).ok());
    EXPECT_FALSE(would_wait);
    EXPECT_EQ(out[17], std::byte{0x5a});
    // One no-wait read spends the warmth: the next would wait again.
    ASSERT_TRUE(device.ReadPageNoWait(3, out, &would_wait).ok());
    EXPECT_TRUE(would_wait);
    // A blocking read is never refused, whatever the mode.
    ASSERT_TRUE(device.ReadPage(3, out).ok());

    device.SetNoWait(NoWait::kAlways);
    device.WarmPage(3);
    ASSERT_TRUE(device.ReadPageNoWait(3, out, &would_wait).ok());
    EXPECT_TRUE(would_wait) << "kAlways is the page only a synchronous read gets";

    // Past the capacity is `ReadPage`'s refusal, never a wait.
    would_wait = true;
    EXPECT_EQ(device.ReadPageNoWait(99, out, &would_wait).code(), StatusCode::kOutOfRange);
    EXPECT_FALSE(would_wait);
}

TEST(DePageDeviceTest, TheFileDeviceCopiesACachedPageAndAWarmedOneWithoutWaiting) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("kds-de-file-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    const std::string path = (dir / "d.db").string();
    auto opened = FilePageDevice::Open(path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    FilePageDevice& device = *opened.value();
    ASSERT_TRUE(device.EnsureCapacity(64).ok());
    std::array<std::byte, kPageSize> in{};
    for (std::size_t i = 0; i < kPageSize; ++i) in[i] = static_cast<std::byte>(i * 7 + 3);
    ASSERT_TRUE(device.WritePage(5, in).ok());
    ASSERT_TRUE(device.Sync().ok());

    // Just written, so the operating system holds it: copied where it stands.
    std::array<std::byte, kPageSize> out{};
    bool would_wait = true;
    ASSERT_TRUE(device.ReadPageNoWait(5, out, &would_wait).ok());
    ASSERT_FALSE(would_wait);
    EXPECT_EQ(std::memcmp(in.data(), out.data(), kPageSize), 0);

    // Dropped from the cache where the file system allows it (tmpfs does
    // not): the read would wait, `WarmPage` waits for it, and then it does
    // not. Either answer must be truthful - bytes only with `!would_wait`.
    {
        const int fd = ::open(path.c_str(), O_RDONLY);
        ASSERT_GE(fd, 0);
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
    out.fill(std::byte{0});
    ASSERT_TRUE(device.ReadPageNoWait(5, out, &would_wait).ok());
    if (would_wait) {
        device.WarmPage(5);
        ASSERT_TRUE(device.ReadPageNoWait(5, out, &would_wait).ok());
        // The page was read a moment ago; a host that evicted it again in
        // between is not this cell's subject.
        if (would_wait) GTEST_SKIP() << "the page cache dropped the page between two calls";
    }
    EXPECT_EQ(std::memcmp(in.data(), out.data(), kPageSize), 0);

    would_wait = true;
    EXPECT_EQ(device.ReadPageNoWait(9999, out, &would_wait).code(), StatusCode::kOutOfRange);
    EXPECT_FALSE(would_wait);
    device.WarmPage(9999);  // past the end: nothing, and no crash
}

// ---- The store's refusing arm -------------------------------------------------

class DeStoreTest : public ::testing::Test {
protected:
    const WithoutFrameBudgetOverride exact_capacity_;  // residency is the subject

    void Build(std::size_t pages, std::size_t capacity) {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/256, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        {
            auto setup = DevicePageStore::Open(*device_, FrameCapacity{pages + 64}, 16);
            ASSERT_TRUE(setup.ok()) << setup.status().message();
            for (std::size_t i = 0; i < pages; ++i) {
                auto made = setup.value()->CreateNew();
                ASSERT_TRUE(made.ok()) << made.status().message();
                FormatPage(made.value().second.bytes(), PageType::kHeap);
                ids_.push_back(made.value().first);
            }
            ASSERT_TRUE(setup.value()->Sync().ok());
        }
        auto store = DevicePageStore::Open(*device_, FrameCapacity{capacity}, 16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
    }

    void TearDown() override { ClearMissRefusal(); }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
    std::vector<PageId> ids_;
};

TEST_F(DeStoreTest, AnArmedMissThatWouldWaitIsRefusedNotedAndHoldsNoFrame) {
    Build(/*pages=*/8, /*capacity=*/64);
    device_->SetNoWait(NoWait::kUntilWarmed);
    const std::size_t resident_before = store_->resident_pages();

    // Not armed: a blocking read, as before DE.
    ASSERT_TRUE(store_->GetForRead(ids_[0]).ok());
    EXPECT_FALSE(MissRefusalNoted());
    EXPECT_EQ(store_->pool_counters().would_wait, 0u);

    ArmMissRefusal();
    auto refused = store_->GetForRead(ids_[1]);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kNotResident);
    EXPECT_TRUE(MissRefusalNoted());
    EXPECT_EQ(MissRefusalPage(), ids_[1]);
    EXPECT_EQ(store_->pool_counters().would_wait, 1u);
    EXPECT_EQ(store_->resident_pages(), resident_before + 1);
    // The slot reserved for the refused read went back: every slot the pool
    // holds is a resident page's or free.
    const auto slots = store_->frame_slots();
    EXPECT_EQ(slots.slots - slots.free, store_->resident_pages())
        << "a refused read kept its reserved slot";

    // The first page refused is the one kept.
    ASSERT_FALSE(store_->GetForRead(ids_[2]).ok());
    EXPECT_EQ(MissRefusalPage(), ids_[1]);

    // A hit is never asked about: page 0 is resident and the device is not
    // consulted, armed or not.
    const std::uint64_t answers = device_->would_wait_answers();
    ASSERT_TRUE(store_->GetForRead(ids_[0]).ok());
    EXPECT_EQ(device_->would_wait_answers(), answers);

    // Warmed, the same armed fetch is admitted - one reservation, one read.
    device_->WarmPage(ids_[1]);
    auto admitted = store_->GetForRead(ids_[1]);
    ASSERT_TRUE(admitted.ok()) << admitted.status().message();
    EXPECT_EQ(store_->resident_pages(), resident_before + 2);

    // A write fetch under the arm is refused the same way: the arm is the
    // thread's, and what may be armed is the dispatcher's to decide.
    auto write = store_->Get(ids_[3]);
    ASSERT_FALSE(write.ok());
    EXPECT_EQ(write.status().code(), StatusCode::kNotResident);

    DisarmMissRefusal();
    ASSERT_TRUE(store_->Get(ids_[3]).ok());
}

TEST_F(DeStoreTest, TheWalksAccessKindIsRefusedWithoutTheArmAndLeavesNoNote) {
    Build(/*pages=*/8, /*capacity=*/64);
    device_->SetNoWait(NoWait::kUntilWarmed);

    auto refused = store_->Fetch(ids_[0], PageAccess::kScanNoWait);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kNotResident);
    EXPECT_FALSE(MissRefusalNoted()) << "the walk yields: it may leave the thread no word";
    EXPECT_EQ(store_->pool_counters().would_wait, 1u);

    // `kScan` beside it is never refused.
    ASSERT_TRUE(store_->Fetch(ids_[1], PageAccess::kScan).ok());

    // The walk's second fetch, after its park: the page is there.
    device_->WarmPage(ids_[0]);
    ASSERT_TRUE(store_->Fetch(ids_[0], PageAccess::kScanNoWait).ok());
    // And a hit under the refusing kind is a hit.
    ASSERT_TRUE(store_->Fetch(ids_[0], PageAccess::kScanNoWait).ok());
}

TEST_F(DeStoreTest, AReadsFailureUnderTheArmIsTheStatusASynchronousReadGives) {
    // DE-R6: an I/O error, a checksum failure and a page never written each
    // reach the caller as today's status, armed or not.
    Build(/*pages=*/8, /*capacity=*/64);
    ArmMissRefusal();

    device_->FailNextRead(Status::IoError("injected"));
    auto io = store_->GetForRead(ids_[0]);
    ASSERT_FALSE(io.ok());
    EXPECT_EQ(io.status().code(), StatusCode::kIoError);
    EXPECT_FALSE(MissRefusalNoted()) << "an error is not a refusal";

    // A torn image: the checksum fails on the miss path, armed as unarmed.
    std::array<std::byte, kPageSize> page{};
    ASSERT_TRUE(device_->ReadPage(ids_[1], page).ok());
    page[kPageSize - 1] ^= std::byte{0xff};
    ASSERT_TRUE(device_->WritePage(ids_[1], page).ok());
    auto torn = store_->GetForRead(ids_[1]);
    ASSERT_FALSE(torn.ok());
    EXPECT_EQ(torn.status().code(), StatusCode::kCorruption);
    EXPECT_FALSE(MissRefusalNoted());
}

TEST_F(DeStoreTest, AFullPoolUnderTheArmIsThePoolsRefusalNotANote) {
    // DE-R6: the reservation is made before the device is asked, so a pool
    // with every frame pinned answers `ResourceExhausted`, as unarmed.
    constexpr std::size_t kCapacity = 16;
    Build(/*pages=*/kCapacity + 1, kCapacity);
    device_->SetNoWait(NoWait::kUntilWarmed);
    store_->SetLatchArmed(store_->latch_armed(), /*concurrent_pinners=*/2);
    std::vector<PageRef> held;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        auto ref = store_->GetForRead(ids_[i]);
        ASSERT_TRUE(ref.ok()) << ref.status().message();
        held.push_back(std::move(ref.value()));
    }
    ArmMissRefusal();
    auto refused = store_->GetForRead(ids_[kCapacity]);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(MissRefusalNoted());
    EXPECT_EQ(device_->would_wait_answers(), 0u) << "the device was asked for a frame nobody had";
}

// ---- The reader ---------------------------------------------------------------

// Counts the wakes a reader writes, and for which core.
class CountingWakes final : public sched::WakeRegistry {
public:
    void Register(std::uint32_t, const std::atomic<bool>*, const sched::Waker*) override {}
    void Kick(std::uint32_t) const noexcept override { kicks_.fetch_add(1); }
    void Wake(std::uint32_t core) const noexcept override {
        last_core_.store(core);
        wakes_.fetch_add(1);
    }
    std::uint64_t kicks() const noexcept override { return kicks_.load(); }
    std::uint64_t wakes() const noexcept { return wakes_.load(); }
    std::uint32_t last_core() const noexcept { return last_core_.load(); }

private:
    mutable std::atomic<std::uint64_t> kicks_{0};
    mutable std::atomic<std::uint64_t> wakes_{0};
    mutable std::atomic<std::uint32_t> last_core_{~0U};
};

template <typename Pred>
bool Within(std::chrono::milliseconds limit, Pred pred) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= until) return pred();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

TEST(DePageReaderTest, AWakeIsWrittenToAReactorTheFlagCallsAwake) {
    // DE-Q14: `Kick` reads the destination's sleeping flag and skips a
    // reactor that is not asleep - the window a finished read can land in.
    // `Wake` writes whatever the flag says.
    auto backend = sched::EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok()) << backend.status().message();
    sched::SystemClock clock;
    sched::WakerTable table(/*core_count=*/1);
    sched::Scheduler scheduler(clock, backend.value());
    ASSERT_TRUE(scheduler.AttachWakerTable(&table, /*core_id=*/0, /*publish_load=*/false).ok());

    table.Kick(0);
    EXPECT_EQ(table.kicks(), 0u) << "nothing runs this reactor: its flag was never raised";
    table.Wake(0);
    EXPECT_EQ(table.kicks(), 1u) << "the unconditional wake was skipped";
    EXPECT_EQ(scheduler.wakes_received(), 1u);
    table.Wake(7);  // outside the table: nothing, and no crash
    EXPECT_EQ(table.kicks(), 1u);
}

TEST(DePageReaderTest, WithNoThreadTheReadIsMadeAtTheRequest) {
    auto made = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_TRUE(made.ok());
    MemoryPageDevice& device = *made.value();
    device.SetNoWait(NoWait::kUntilWarmed);
    PageReader reader(device);
    EXPECT_FALSE(reader.threaded());

    const PageReader::Ticket ticket = reader.Submit(4, /*core=*/0);
    EXPECT_TRUE(ticket.done());
    EXPECT_EQ(reader.submitted(), 0u) << "nothing was handed to a thread";
    std::array<std::byte, kPageSize> out{};
    bool would_wait = true;
    ASSERT_TRUE(device.ReadPageNoWait(4, out, &would_wait).ok());
    EXPECT_FALSE(would_wait) << "the request warmed the page";
}

TEST(DePageReaderTest, AThreadReadsThePageMarksTheTicketAndWakesTheAskersCore) {
    auto made = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/8);
    ASSERT_TRUE(made.ok());
    MemoryPageDevice& device = *made.value();
    device.SetNoWait(NoWait::kUntilWarmed);
    CountingWakes wakes;
    PageReader reader(device);
    reader.Start(2, &wakes);
    EXPECT_TRUE(reader.threaded());

    const PageReader::Ticket ticket = reader.Submit(6, /*core=*/3);
    ASSERT_TRUE(Within(std::chrono::seconds(10), [&] { return ticket.done(); }));
    ASSERT_TRUE(Within(std::chrono::seconds(10), [&] { return wakes.wakes() == 1; }));
    EXPECT_EQ(wakes.last_core(), 3u);
    EXPECT_EQ(wakes.kicks(), 0u) << "the wake is the unconditional one (DE-Q14)";
    EXPECT_EQ(reader.completed(), 1u);
    std::array<std::byte, kPageSize> out{};
    bool would_wait = true;
    ASSERT_TRUE(device.ReadPageNoWait(6, out, &would_wait).ok());
    EXPECT_FALSE(would_wait);

    // A ticket dropped before its read is made changes nothing: the read
    // needs no owner (a cancelled asker, a closed session).
    { (void)reader.Submit(7, /*core=*/1); }
    ASSERT_TRUE(Within(std::chrono::seconds(10), [&] { return reader.completed() == 2; }));

    reader.Stop();
    EXPECT_FALSE(reader.threaded());
    // Stopped, a request is served where it is made again.
    EXPECT_TRUE(reader.Submit(2, /*core=*/0).done());
}

}  // namespace
}  // namespace kds::storage

// ---- A top-level SELECT's head, discarded and run again ----------------------------

namespace kds::sim {
namespace {

using storage::MemoryPageDevice;

// About four rows a leaf, so `kRows` rows are several hundred leaves against
// a pool at the harness's smallest cap: a point read usually misses.
constexpr int kRows = 1200;
constexpr std::size_t kFrames = 256;

std::uint64_t MissReruns() { return Contention::Read().of(Tally::kMissReruns); }

class DeSelectRerunTest : public ::testing::Test {
protected:
    const WithoutFrameBudgetOverride exact_capacity_;  // residency is the subject

    static std::unique_ptr<SimInstance> Make(bool read_off_reactor) {
        SimInstance::Options options;
        options.buffer_pool_frames = kFrames;
        options.read_off_reactor = read_off_reactor;
        options.access_statistics = true;
        auto made = SimInstance::Create(options);
        EXPECT_TRUE(made.ok()) << made.status().message();
        return made.ok() ? std::move(made.value()) : nullptr;
    }

    static void Load(SimInstance& db) {
        ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, grp int64, name varchar(2000))")
                      .rfind("CREATED", 0),
                  0u);
        ASSERT_EQ(db.Execute("CREATE INDEX t_grp ON t (grp)").rfind("ERR", 0), std::string::npos);
        ASSERT_EQ(db.Execute("CREATE TABLE side (id int64, t_id int64)").rfind("CREATED", 0), 0u);
        ASSERT_TRUE(db.RunCheckpoint().ok());
        const std::string wide(1900, 'x');
        for (int at = 1; at <= kRows;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int n = 0; n < 40 && at <= kRows; ++n, ++at) {
                if (n != 0) sql += ", ";
                sql += "(" + std::to_string(at) + ", " + std::to_string(at % 97) + ", '" + wide +
                       std::to_string(at) + "')";
            }
            const std::string reply = db.Execute(sql);
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
            // A pool this small holds few dirty pages: written back as the
            // load goes, as a server's cadence does.
            if (at % 120 == 1) {
                ASSERT_TRUE(db.RunCheckpoint().ok());
            }
        }
        for (int i = 1; i <= 50; ++i) {
            ASSERT_EQ(db.Execute("INSERT INTO side VALUES (" + std::to_string(i) + ", " +
                                 std::to_string(i * 37) + ")")
                          .rfind("ERR", 0),
                      std::string::npos);
        }
        ASSERT_TRUE(db.RunCheckpoint().ok());
    }

    // The statements whose answers must not depend on the path: descents of
    // every inline kind, present and absent keys, and a head in each kind of
    // transaction.
    static std::vector<std::string> Reads() {
        std::vector<std::string> sql;
        for (int k : {1, 7, 401, 977, 603, 1199, 1200, 1201, 99999}) {
            sql.push_back("SELECT id, grp FROM t WHERE id = " + std::to_string(k));
        }
        sql.push_back("SELECT id FROM t WHERE id IN (3, 811, 622, 1190, 5000)");
        sql.push_back("SELECT id FROM t WHERE grp = 13 ORDER BY id LIMIT 5");
        sql.push_back("SELECT side.id, t.grp FROM side JOIN t ON t.id = side.t_id WHERE side.id = 9");
        sql.push_back("SELECT name FROM t WHERE id = 500");
        sql.push_back("BEGIN");
        sql.push_back("SELECT id FROM t WHERE id = 313");
        sql.push_back("SELECT id FROM t WHERE id = 1222");
        sql.push_back("COMMIT");
        sql.push_back("BEGIN READ ONLY");
        sql.push_back("SELECT grp FROM t WHERE id = 717");
        sql.push_back("COMMIT");
        return sql;
    }
};

TEST_F(DeSelectRerunTest, ARefusedHeadAnswersWhatASynchronousOneAnswers) {
    // DE-R1, and DE-Q10's discard: the two instances differ only in whether
    // a point read may be refused a page, and every reply is the same - a
    // caller that swallowed a refusal on the way (the bind's `StillOwned`,
    // a hint's verify) answered into a run nobody saw.
    auto plain = Make(/*read_off_reactor=*/false);
    auto armed = Make(/*read_off_reactor=*/true);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(armed, nullptr);
    Load(*plain);
    Load(*armed);

    const std::uint64_t reruns_before = MissReruns();
    for (int pass = 0; pass < 3; ++pass) {
        for (const std::string& sql : Reads()) {
            const std::string want = plain->Execute(sql);
            const std::string got = armed->Execute(sql);
            EXPECT_EQ(got, want) << sql;
            // The arm was the body's: dropped before the statement's tail
            // and never left for what this thread runs next (DE-Q10 (ii)).
            EXPECT_FALSE(storage::MissRefusalArmed()) << sql;
            EXPECT_FALSE(storage::MissRefusalNoted()) << sql;
            EXPECT_EQ(got.find("NOT_RESIDENT"), std::string::npos) << sql;
            EXPECT_EQ(got.find("waited for"), std::string::npos)
                << "the refusal reached a client: " << sql << " -> " << got;
        }
    }
    EXPECT_GT(MissReruns(), reruns_before)
        << "no head was refused a page, so the run-again was never exercised";
    EXPECT_GT(armed->page_device().would_wait_answers(), 0u);
    EXPECT_EQ(plain->page_device().would_wait_answers(), 0u);
}

TEST_F(DeSelectRerunTest, ADiscardedRunRecordsNothing) {
    // DE-Q10 (ii): the access statistics are written at a `SELECT`'s tail,
    // inside the head. A discarded run that recorded would count its shape
    // once more than the statement ran.
    auto plain = Make(false);
    auto armed = Make(true);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(armed, nullptr);
    Load(*plain);
    Load(*armed);
    const std::uint64_t reruns_before = MissReruns();
    for (int k = 5; k < kRows; k += 61) {
        const std::string sql = "SELECT id FROM t WHERE id = " + std::to_string(k);
        ASSERT_EQ(armed->Execute(sql), plain->Execute(sql)) << sql;
    }
    ASSERT_GT(MissReruns(), reruns_before);
    EXPECT_EQ(armed->Execute("SHOW ACCESS"), plain->Execute("SHOW ACCESS"));
}

// A session sink that counts: rows handed to it, and rewinds.
class CountingSink final : public server::ResultSink {
public:
    Status Describe(std::vector<wire::FieldDescription> fields) override {
        return text_.Describe(std::move(fields));
    }
    Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                              std::span<const std::uint32_t> types, const exec::ChainFrame& frame,
                              std::string& out) override {
        return text_.EncodeProjectedRow(projection, types, frame, out);
    }
    Status EncodeValueRow(std::span<const std::uint32_t> types,
                          std::span<const parser::AstValue> values, std::string& out) override {
        return text_.EncodeValueRow(types, values, out);
    }
    Status Emit(std::string_view row) override {
        rows.emplace_back(row);
        return Status::OK();
    }
    void Rewind() override {
        ++rewinds;
        rows.clear();
    }
    std::vector<std::string> rows;
    int rewinds = 0;

private:
    server::TextResultSink text_;
};

TEST_F(DeSelectRerunTest, ARunWhoseRefusalACallerSwallowedIsDiscardedWhole) {
    // DE-Q10, `[quiet-wrong]`: thirty-odd callers recover from a failed
    // fetch, and one that did would let the body finish and answer. The
    // cell plants exactly that - a refusal noted mid-body, in a run that
    // goes on to return OK with its row emitted - and the dispatcher must
    // obey the note: the row taken back, nothing recorded for the run, the
    // statement made again, and its answer the one a plain run gives.
    auto plain = Make(false);
    auto armed = Make(true);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(armed, nullptr);
    Load(*plain);
    Load(*armed);
    const std::string sql = "SELECT id, grp FROM t WHERE id = 640";
    // Both instances have run the shape once, with its pages now resident.
    ASSERT_EQ(armed->Execute(sql), plain->Execute(sql));

    CountingSink sink;
    armed->session().set_result_sink(&sink);
    int planted = 0;
    armed->dispatcher().SetSelectRowEmittedForTest([&](std::uint64_t) {
        if (planted++ == 0) storage::NoteMissRefusal(/*page_id=*/1);
        return Status::OK();
    });
    const std::uint64_t reruns_before = MissReruns();
    const std::string reply = armed->Execute(sql);
    armed->dispatcher().SetSelectRowEmittedForTest({});
    armed->session().set_result_sink(nullptr);

    EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    EXPECT_EQ(MissReruns() - reruns_before, 1u) << "the planted refusal was not obeyed";
    EXPECT_EQ(planted, 2) << "the statement was not run again";
    EXPECT_EQ(sink.rewinds, 1) << "the discarded run's row was not taken back";
    ASSERT_EQ(sink.rows.size(), 1u) << "the client would have read the row twice, or not at all";
    EXPECT_EQ(sink.rows[0], "640," + std::to_string(640 % 97));
    // The statement ran twice on the armed instance as far as anything
    // recorded knows - the first run above and this one's answer - so a
    // plain instance that ran it twice has the same counts: the discarded
    // run left none behind.
    ASSERT_EQ(plain->Execute(sql).rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(armed->Execute("SHOW ACCESS"), plain->Execute("SHOW ACCESS"));
}

TEST_F(DeSelectRerunTest, TheRunAfterTheEighthRefusedOneReadsSynchronously) {
    // DE-R7, DE-Q11: a page no read thread can ever deliver - every no-wait
    // read would wait - is refused eight times and then read on the core,
    // as today. A slow answer, never a refused one.
    auto armed = Make(true);
    ASSERT_NE(armed, nullptr);
    Load(*armed);
    armed->page_device().SetNoWait(MemoryPageDevice::NoWait::kAlways);
    // A walk run here is not armed and reads synchronously; it turns the
    // pool over, so the descent below finds its leaf outside it.
    ASSERT_EQ(armed->Execute("SELECT id FROM t WHERE grp = -1").rfind("ERR", 0), std::string::npos);

    const std::uint64_t reruns_before = MissReruns();
    const std::string got = armed->Execute("SELECT id, grp FROM t WHERE id = 42");
    EXPECT_EQ(got.rfind("ERR", 0), std::string::npos) << got;
    EXPECT_NE(got.find("42,42"), std::string::npos) << got;
    EXPECT_EQ(MissReruns() - reruns_before, server::CommandDispatcher::kMaxMissReruns);
}

TEST_F(DeSelectRerunTest, NothingButATopLevelSelectIsEverRefused) {
    // DE-R2, DE-Q7 (a), DE-Q10 (i): a write, a locking read, a `SELECT`
    // inside a write and a traced `SELECT` read every page synchronously,
    // however cold - the device is never asked whether a read would wait.
    auto armed = Make(true);
    ASSERT_NE(armed, nullptr);
    Load(*armed);
    const auto flood = [&] {
        ASSERT_EQ(armed->Execute("SELECT id FROM t WHERE grp = -1").rfind("ERR", 0),
                  std::string::npos);
    };
    const std::vector<std::string> never = {
        "UPDATE t SET grp = grp + 1000 WHERE id = 77",
        "DELETE FROM t WHERE id = 1134",
        "INSERT INTO t VALUES (50001, 5, 'n')",
        "INSERT INTO side SELECT id + 1000, grp FROM t WHERE id = 911",
        "UPDATE side SET t_id = (SELECT grp FROM t WHERE id = 888) WHERE id = 3",
        "ANALYZE SELECT id FROM t WHERE id = 501",
    };
    for (const std::string& sql : never) {
        flood();
        const std::uint64_t asked = armed->page_device().would_wait_answers();
        const std::uint64_t reruns = MissReruns();
        const std::string got = armed->Execute(sql);
        EXPECT_EQ(got.rfind("ERR", 0), std::string::npos) << sql << " -> " << got;
        EXPECT_EQ(armed->page_device().would_wait_answers(), asked) << sql;
        EXPECT_EQ(MissReruns(), reruns) << sql;
    }
    // A locking read inside its transaction, the same.
    flood();
    ASSERT_EQ(armed->Execute("BEGIN").rfind("ERR", 0), std::string::npos);
    const std::uint64_t asked = armed->page_device().would_wait_answers();
    EXPECT_EQ(armed->Execute("SELECT id FROM t WHERE id = 601 FOR UPDATE").rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(armed->page_device().would_wait_answers(), asked);
    ASSERT_EQ(armed->Execute("COMMIT").rfind("ERR", 0), std::string::npos);

    // And the writes landed where a later read finds them.
    EXPECT_NE(armed->Execute("SELECT grp FROM t WHERE id = 77").find("1077"), std::string::npos);
    EXPECT_EQ(armed->Execute("SELECT id FROM t WHERE id = 1134").find("1134"), std::string::npos);
}

TEST_F(DeSelectRerunTest, EveryDeviceReadIsOnePageReadEventOnEitherPath) {
    // DE-R9: a refused attempt books nothing, the wait for the page is the
    // event, and the copy after it is not a second one - so over any run of
    // reads the events counted equal the reads the device made.
    for (const bool read_off_reactor : {false, true}) {
        auto db = Make(read_off_reactor);
        ASSERT_NE(db, nullptr);
        Load(*db);
        // This thread's own totals: it is no reactor, so its waits are in
        // no core's row of `sys.wait_stats`. A spilled value's page is
        // `io.var_heap_read`, renamed from the same scope.
        const auto events = []() -> std::uint64_t {
            const sched::CoreWaitTotals& totals = sched::WaitTotals();
            std::uint64_t n = 0;
            for (const stats::WaitEvent e :
                 {stats::WaitEvent::kIoPageRead, stats::WaitEvent::kIoVarHeapRead}) {
                n += totals.totals[static_cast<std::size_t>(e)].count.get();
            }
            return n;
        };
        const std::uint64_t events_before = events();
        const std::uint64_t reads_before = db->page_device().stats().reads;
        const std::uint64_t reruns_before = MissReruns();
        for (int k = 11; k < kRows; k += 53) {
            ASSERT_EQ(db->Execute("SELECT id FROM t WHERE id = " + std::to_string(k))
                          .rfind("ERR", 0),
                      std::string::npos);
        }
        const std::uint64_t reads = db->page_device().stats().reads - reads_before;
        ASSERT_GT(reads, 0u) << "no read missed the pool";
        if (read_off_reactor) ASSERT_GT(MissReruns(), reruns_before);
        EXPECT_EQ(events() - events_before, reads)
            << (read_off_reactor ? "refusing" : "synchronous") << " path";
    }
}

}  // namespace
}  // namespace kds::sim
