// **A read that would wait leaves its reactor: the park** (DE-S1,
// `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md`).
//
// The whole engine on the two-core rig, a pool far smaller than the
// relation, the memory device answering "would wait" for every page a
// reader has not named, and a page reader with real threads waking the
// reactors through the rig's real table. `de_page_read_test.cpp` holds the
// rules a single thread can show; these cells hold what needs a reactor:
//
// - a point `SELECT` whose page would wait parks, is read by a thread, is
//   woken and answers - on both cores at once, over the same keys, so two
//   cores ask for one absent page and both answer;
// - a wait that outlasts the statement's deadline, and one whose client
//   cancels, end in a synchronous read - a slow answer, never a refused one
//   (DE-R7) - and the read left behind finishes into nothing.
//
// The reactors run under the suspend audit and strict wait scopes
// (`CoreRuntime::Run`), so a park that held a pin or a span, or one outside
// a wait scope, would abort the cell (DE-R3).

#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "frame_budget_override.hpp"
#include "kds/base/contention.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_reader.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

constexpr int kRows = 1200;          // ~300 leaves of ~2 KB rows
constexpr std::size_t kFrames = 320;  // a fraction of them

struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::vector<DispatchOutcome> outs;
    std::atomic<std::size_t> done{0};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

// A device for the reader alone: `WarmPage` waits at a gate, then warms the
// rig's device. What a read that the device does not finish looks like.
class GatedWarm final : public storage::PageDevice {
public:
    explicit GatedWarm(storage::MemoryPageDevice& inner) : inner_(inner) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        return inner_.WritePage(id, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override { return inner_.Sync(); }
    void WarmPage(PageId id) noexcept override {
        {
            std::unique_lock lock(mu_);
            ++waiting_;
            cv_.wait(lock, [this] { return open_; });
        }
        inner_.WarmPage(id);
    }
    void Open() {
        {
            std::lock_guard lock(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }
    int waiting() const {
        std::lock_guard lock(mu_);
        return waiting_;
    }

private:
    storage::MemoryPageDevice& inner_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
    int waiting_ = 0;
};

class DePageReadRigTest : public ::testing::Test {
protected:
    const WithoutFrameBudgetOverride exact_capacity_;  // residency is the subject

    // `idle_block_ms`: long by default, so a park that is not woken fails
    // its cell; a cell whose park ends on a deadline sets it short, since a
    // deadline is read when the reactor turns.
    void Open(int idle_block_ms = 5000) {
        TwoCoreRig::Options options;
        options.max_idle_block_ms = idle_block_ms;
        options.buffer_pool_frames = kFrames;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        CommandDispatcher& d = rig_->core(0).dispatcher();
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, grp int64, name varchar(2000))")
                      .response.rfind("CREATED", 0),
                  0u);
        const std::string wide(1900, 'x');
        for (int at = 1; at <= kRows;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int n = 0; n < 40 && at <= kRows; ++n, ++at) {
                if (n != 0) sql += ", ";
                sql += "(" + std::to_string(at) + ", " + std::to_string(at % 97) + ", '" + wide +
                       "')";
            }
            const std::string reply = d.Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
            // Written back as the load goes: a pool this small cannot hold
            // the relation dirty.
            ASSERT_TRUE(rig_->wal().DrainOnce().ok());
            ASSERT_TRUE(rig_->store().Sync().ok());
        }
        ASSERT_NE(rig_->memory_device(), nullptr);
    }

    void TearDown() override {
        // In this order, whatever the cell left: a gate still closed would
        // hold the reader's thread, and so its join, for good; the reader's
        // threads wake cores, so they stop before a reactor goes; and no
        // dispatcher keeps a reader that is about to die.
        if (gated_.has_value()) gated_->Open();
        if (reader_.has_value()) reader_->Stop();
        if (rig_ != nullptr) {
            rig_->Stop();
            rig_->core(0).dispatcher().set_page_reader(nullptr);
            rig_->core(1).dispatcher().set_page_reader(nullptr);
        }
    }

    // Arms the device and both cores' dispatchers with a reader of
    // `threads` threads - over the rig's device, or over a gate in front of
    // it (`gated`), whose reads finish only once a cell opens it.
    void Arm(int threads, bool gated = false) {
        rig_->memory_device()->SetNoWait(storage::MemoryPageDevice::NoWait::kUntilWarmed);
        if (gated) {
            gated_.emplace(*rig_->memory_device());
            reader_.emplace(*gated_);
        } else {
            reader_.emplace(rig_->device());
        }
        reader_->Start(static_cast<unsigned>(threads), &rig_->wakers());
        rig_->core(0).dispatcher().set_page_reader(&*reader_);
        rig_->core(1).dispatcher().set_page_reader(&*reader_);
    }

    void Submit(std::uint32_t core, Script& script) {
        rig_->core(core).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig_->core(core).dispatcher(), script)));
    }

    static std::string Row(int k) { return std::to_string(k) + "," + std::to_string(k % 97); }

    std::unique_ptr<TwoCoreRig> rig_;
    // The fixture's, not a cell's: both outlive the reactors that use them.
    std::optional<GatedWarm> gated_;
    std::optional<storage::PageReader> reader_;
};

TEST_F(DePageReadRigTest, AReadThatWouldWaitParksIsReadByAThreadAndAnswersOnBothCores) {
    Open();
    Arm(2);
    storage::PageReader& reader = *reader_;
    const std::uint64_t reruns_before = Contention::Read().of(Tally::kMissReruns);

    Session s0, s1;
    Script a{&s0}, b{&s1};
    std::vector<int> keys;
    for (int k = 3; k <= kRows; k += 29) keys.push_back(k);
    for (Script* s : {&a, &b}) {
        for (int k : keys) {
            s->lines.push_back("SELECT id, grp FROM t WHERE id = " + std::to_string(k));
        }
        s->lines.push_back("SELECT id, grp FROM t WHERE id = 999999");
    }
    Submit(0, a);
    Submit(1, b);
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] {
        return a.done.load() == a.lines.size() && b.done.load() == b.lines.size();
    })) << "a parked statement was never run again: core 0 at " << a.done.load() << ", core 1 at "
        << b.done.load();

    for (const Script* s : {&a, &b}) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::string& reply = s->outs[i].response;
            EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
            EXPECT_NE(reply.find(Row(keys[i])), std::string::npos)
                << s->lines[i] << " -> " << reply;
        }
        const std::string& absent = s->outs.back().response;
        EXPECT_EQ(absent.rfind("ERR", 0), std::string::npos) << absent;
        EXPECT_EQ(absent.find("999999,"), std::string::npos) << absent;
    }
    EXPECT_GT(Contention::Read().of(Tally::kMissReruns), reruns_before)
        << "no statement was refused a page, so no park was taken";
    EXPECT_GT(reader.completed(), 0u) << "no read was made by a thread";
    EXPECT_GT(rig_->store().pool_counters().would_wait, 0u);
}

TEST_F(DePageReadRigTest, AWaitPastTheDeadlineEndsInASynchronousReadNotARefusal) {
    // DE-R7. The reader's device never finishes the read; the statement's
    // deadline - the lock family's fault net, set short - ends the park, no
    // later run of the statement is refused, and it answers.
    Open();
    Arm(1, /*gated=*/true);
    GatedWarm& gated = *gated_;
    storage::PageReader& reader = *reader_;
    rig_->core(0).dispatcher().set_lock_wait_fault_net_ns(200'000'000);  // 200 ms
    const std::uint64_t reruns_before = Contention::Read().of(Tally::kMissReruns);

    Session s0;
    Script a{&s0};
    a.lines.push_back("SELECT id, grp FROM t WHERE id = 17");
    Submit(0, a);
    rig_->Start();
    ASSERT_TRUE(Within(30000ms, [&] { return a.done.load() == 1; }))
        << "the statement outlived its deadline parked";
    EXPECT_EQ(gated.waiting(), 1) << "the read was never handed to the thread";
    EXPECT_NE(a.outs[0].response.find(Row(17)), std::string::npos) << a.outs[0].response;
    EXPECT_EQ(Contention::Read().of(Tally::kMissReruns) - reruns_before, 1u)
        << "after the deadline no run of the statement may be refused again";
    // The read left behind finishes into nothing.
    gated.Open();
    ASSERT_TRUE(Within(10000ms, [&] { return reader.completed() == 1; }));
}

TEST_F(DePageReadRigTest, ACancelEndsTheParkAndTheStatementReadsSynchronously) {
    Open();
    Arm(1, /*gated=*/true);
    GatedWarm& gated = *gated_;
    // A deadline the cell would time out before: only the cancel ends the park.
    rig_->core(0).dispatcher().set_lock_wait_fault_net_ns(120'000'000'000);

    Session s0;
    Script a{&s0};
    a.lines.push_back("SELECT id, grp FROM t WHERE id = 23");
    Submit(0, a);
    rig_->Start();
    ASSERT_TRUE(Within(30000ms, [&] { return gated.waiting() == 1; }))
        << "the statement never parked on its read";
    EXPECT_EQ(a.done.load(), 0u);
    s0.cancel_flag()->store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig_, 0, [&] { return a.done.load() == 1; }, 30000ms))
        << "the cancel did not end the park";
    // A descent takes no cancel (it is taken at a walk's slice boundary or
    // at the next frame): the statement answers, having read its page itself.
    EXPECT_NE(a.outs[0].response.find(Row(23)), std::string::npos) << a.outs[0].response;
    gated.Open();
}

// ---- DE-S2: the walk ---------------------------------------------------------

TEST_F(DePageReadRigTest, AWalkParksAtALeafThatWouldWaitAndAnswersEveryRowOnce) {
    // DE-Q8, DE-R1. A walk handed on fetches each leaf refusing a wait; a
    // leaf the device cannot hand over is read by a thread while the walk
    // is parked, then fetched again. Every row is answered, once - a
    // refused leaf is neither skipped nor visited twice - and the walk is
    // never run again from its head.
    Open();
    Arm(2);
    const std::uint64_t reruns_before = Contention::Read().of(Tally::kMissReruns);
    const std::uint64_t refused_before = rig_->store().pool_counters().would_wait;

    Session s0, s1;
    Script a{&s0}, b{&s1};
    a.lines.push_back("SELECT id FROM t WHERE grp = 5");
    b.lines.push_back("SELECT id FROM t WHERE grp = 96");
    Submit(0, a);
    Submit(1, b);
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] { return a.done.load() == 1 && b.done.load() == 1; }))
        << "a parked walk was never resumed";

    const auto expect = [](const std::string& reply, int grp) {
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        std::vector<std::string> rows;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;) {
            const std::size_t next = reply.find("\\n", at + 2);
            rows.push_back(reply.substr(at + 2, next == std::string::npos ? next : next - at - 2));
            at = next;
        }
        std::vector<std::string> want;
        for (int k = 1; k <= kRows; ++k) {
            if (k % 97 == grp) want.push_back(std::to_string(k));
        }
        std::sort(rows.begin(), rows.end());
        std::sort(want.begin(), want.end());
        EXPECT_EQ(rows, want) << "grp " << grp;
    };
    expect(a.outs[0].response, 5);
    expect(b.outs[0].response, 96);
    EXPECT_GT(rig_->store().pool_counters().would_wait, refused_before)
        << "no leaf was refused, so the walk never parked";
    EXPECT_GT(reader_->completed(), 0u);
    EXPECT_EQ(Contention::Read().of(Tally::kMissReruns), reruns_before)
        << "a walk is resumed where it parked, never run again";
}

TEST_F(DePageReadRigTest, WhileAWalkIsParkedOnItsReadTheCoreRunsOtherStatements) {
    // What DE is for. The reader's device does not finish the walk's read,
    // so the walk stays parked - and the core it would have been stalling
    // creates a relation, writes to it and reads it back meanwhile.
    Open();
    Arm(1, /*gated=*/true);
    // A deadline the cell would time out before: only the gate ends the park.
    rig_->core(0).dispatcher().set_lock_wait_fault_net_ns(120'000'000'000);
    Session s0, s1;
    Script walk{&s0}, other{&s1};
    walk.lines.push_back("SELECT id FROM t WHERE grp = 7");
    other.lines = {"CREATE TABLE u (id int64, v int64)", "INSERT INTO u VALUES (1, 10)",
                   "SELECT id, v FROM u WHERE id = 1"};
    Submit(0, walk);
    rig_->Start();
    ASSERT_TRUE(Within(30000ms, [&] { return gated_->waiting() == 1; }))
        << "the walk never parked on a leaf";
    rig_->Post(0, sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                      RunScript(rig_->core(0).dispatcher(), other)));
    ASSERT_TRUE(Within(30000ms, [&] { return other.done.load() == other.lines.size(); }))
        << "the core ran nothing while the walk waited for its read";
    EXPECT_EQ(walk.done.load(), 0u) << "the walk finished with its read still at the gate";
    EXPECT_NE(other.outs[2].response.find("1,10"), std::string::npos) << other.outs[2].response;

    gated_->Open();
    ASSERT_TRUE(KickUntil(*rig_, 0, [&] { return walk.done.load() == 1; }, 60000ms));
    EXPECT_EQ(walk.outs[0].response.rfind("ERR", 0), std::string::npos) << walk.outs[0].response;
    EXPECT_NE(walk.outs[0].response.find("\\n7"), std::string::npos);
}

TEST_F(DePageReadRigTest, ACancelIsTakenByAWalkParkedOnItsRead) {
    // A walk takes a cancel where it yields (`protocol.md` §10), and a park
    // on a read is such a place: the statement ends `CANCELLED`, as it
    // would have at its next slice boundary.
    Open();
    Arm(1, /*gated=*/true);
    rig_->core(0).dispatcher().set_lock_wait_fault_net_ns(120'000'000'000);
    Session s0;
    Script walk{&s0};
    walk.lines.push_back("SELECT id FROM t WHERE grp = 9");
    Submit(0, walk);
    rig_->Start();
    ASSERT_TRUE(Within(30000ms, [&] { return gated_->waiting() == 1; }));
    s0.cancel_flag()->store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig_, 0, [&] { return walk.done.load() == 1; }, 30000ms))
        << "the cancel did not end the walk's park";
    EXPECT_EQ(walk.outs[0].status.code(), StatusCode::kCancelled) << walk.outs[0].response;
    // Taken at the park, not at the slice boundary after it: the walk
    // fetched no leaf past the one it was parked on, so it asked the reader
    // for no second page.
    EXPECT_EQ(reader_->submitted(), 1u);
}

TEST_F(DePageReadRigTest, AWalksSecondFetchOfALeafIsSynchronous) {
    // DE-Q8, DE-R7. Every read the device is asked for without waiting
    // would wait, whatever a thread has read: the fetch after the park must
    // be the blocking one, or the walk could never get its leaf.
    Open();
    Arm(2);
    rig_->memory_device()->SetNoWait(storage::MemoryPageDevice::NoWait::kAlways);
    Session s0;
    Script walk{&s0};
    walk.lines.push_back("SELECT id FROM t WHERE grp = 11");
    Submit(0, walk);
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] { return walk.done.load() == 1; }));
    const std::string& reply = walk.outs[0].response;
    ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    for (int k = 11; k <= kRows; k += 97) {
        EXPECT_NE(reply.find("\\n" + std::to_string(k)), std::string::npos) << k;
    }
    EXPECT_GT(reader_->completed(), 0u);
}

TEST_F(DePageReadRigTest, AWalksParkPastItsDeadlineEndsInASynchronousRead) {
    // DE-R7. The reader never finishes a read; each park ends at the fault
    // net, set short, and the walk reads its leaf itself and goes on.
    Open(/*idle_block_ms=*/5);
    Arm(1, /*gated=*/true);
    rig_->core(0).dispatcher().set_lock_wait_fault_net_ns(10'000'000);  // 10 ms
    Session s0;
    Script walk{&s0};
    walk.lines.push_back("SELECT id FROM t WHERE id >= 1 AND id <= 40");
    Submit(0, walk);
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] { return walk.done.load() == 1; }))
        << "a walk outlived its parks' deadlines";
    EXPECT_GE(gated_->waiting(), 1) << "the walk never parked";
    EXPECT_EQ(reader_->completed(), 0u) << "the gate never opened: no read was made by a thread";
    const std::string& reply = walk.outs[0].response;
    ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    for (int k = 1; k <= 40; ++k) {
        const std::string row = "\\n" + std::to_string(k);
        EXPECT_TRUE(reply.find(row + "\\n") != std::string::npos || reply.ends_with(row))
            << k << " in " << reply;
    }
}

TEST_F(DePageReadRigTest, AWalkDrivenToItsEndWhereItStandsNeverParks) {
    // A walk handed on and then driven synchronously - the query layer's
    // walks, a write's subquery - can only yield. It must read as it always
    // has: with the reader's threads running and every leaf cold, these
    // statements answer. (Found by DE-S2's review: armed at the hand-off,
    // such a walk was refused "parked on a wait under a synchronous
    // dispatch".)
    Open();
    Arm(2);
    Session s0;
    Script a{&s0};
    a.lines = {
        "SELECT grp, COUNT(*) FROM t GROUP BY grp HAVING COUNT(*) > 12",
        "CREATE TABLE side (id int64, v int64)",
        "INSERT INTO side VALUES (1, 0)",
        "UPDATE side SET v = (SELECT MAX(id) FROM t WHERE grp = 3) WHERE id = 1",
        "SELECT id, v FROM side WHERE id = 1",
    };
    Submit(0, a);
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] { return a.done.load() == a.lines.size(); }));
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        EXPECT_EQ(a.outs[i].response.rfind("ERR", 0), std::string::npos)
            << a.lines[i] << " -> " << a.outs[i].response;
    }
    // 1200 rows over 97 groups: groups 1 to 36 hold 13 rows, the rest 12.
    EXPECT_NE(a.outs[0].response.find("\\n1,13"), std::string::npos) << a.outs[0].response;
    EXPECT_EQ(a.outs[0].response.find("\\n37,"), std::string::npos) << a.outs[0].response;
    // The largest id in group 3 is 3 + 97 * 12 = 1167.
    EXPECT_NE(a.outs[4].response.find("1,1167"), std::string::npos) << a.outs[4].response;
}

}  // namespace
}  // namespace kds::server
