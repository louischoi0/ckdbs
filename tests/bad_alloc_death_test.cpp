// An allocation failure inside a statement stops the process; it does not
// complete the statement as OK (`docs/spec/sched.md` §3). The cells run in
// death tests, since the process is not left usable.

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <vector>

#include <unistd.h>

#include "alloc_counter.hpp"
#include "file_rig_crash.hpp"
#include "kds/base/fail_stop.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/coro.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

using test_support::CountAllocations;
using test_support::FailNthAllocation;

sched::Coro ThrowsBadAlloc() {
    throw std::bad_alloc();
    co_return Status::OK();
}

// Runs a coroutine that never parks; a park would be a hang, so it is bounded.
void Drive(sched::Coro& c) {
    for (int turns = 0; !c.done(); ++turns) {
        ASSERT_LT(turns, 1'000'000) << "the coroutine parked on a wait nothing delivers";
        c.TryResumeDeepest();
    }
}

class BadAllocDeathTest : public ::testing::Test {
protected:
    // Death tests fork; with threads alive from other tests in the binary only
    // the re-exec style is safe.
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(BadAllocDeathTest, AnExceptionEscapingACoroutineStopsTheProcess) {
    EXPECT_DEATH(
        {
            sched::Coro c = ThrowsBadAlloc();
            Drive(c);
        },
        "exception escaped a coroutine");
}

class BadAllocStatementTest : public BadAllocDeathTest {
protected:
    void SetUp() override {
        BadAllocDeathTest::SetUp();
        auto boot = bootstrap::BootstrapDatabase(store_, 7000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, k int64) BTREE")
                      .response.substr(0, 7),
                  "CREATED");
        for (int i = 0; i < 40; ++i) {
            ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ")")
                          .response.substr(0, 8),
                      "INSERTED");
        }
    }

    // One statement through the coroutine entry the default port uses.
    DispatchOutcome RunAsync(const std::string& sql) {
        DispatchOutcome out;
        Session session;
        sched::Coro c = dispatcher_->DispatchAsync(sql, &session, &out);
        Drive(c);
        return out;
    }

    // The statement's allocations once it is warm (compiled, registered): what
    // the failing run, which is also a second run, will make.
    std::size_t AllocationsOf(const std::string& sql) {
        RunAsync(sql);
        CountAllocations counting;
        const DispatchOutcome whole = RunAsync(sql);
        EXPECT_NE(whole.response.find("count(*)"), std::string::npos) << whole.response;
        return counting.count();
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(BadAllocStatementTest, AnAggregatedSelectThatMeetsAFailedAllocationStopsAtTheAllocation) {
    const std::string sql = "SELECT k, COUNT(*) FROM t GROUP BY k";
    const std::size_t total = AllocationsOf(sql);
    ASSERT_GT(total, 8u);

    // A server's `main` installs the new-handler: the stop is at the failing
    // allocation, before `std::bad_alloc` is thrown and anything unwinds.
    // Inside the statement, past the coroutine's own frame, at three depths.
    for (const std::size_t at : {total / 4, total / 2, (total * 3) / 4}) {
        EXPECT_DEATH(
            {
                base::InstallAllocationFailStop();
                FailNthAllocation fail(at);
                RunAsync(sql);
                // Reaching here is the defect: the statement came back.
                std::exit(1);
            },
            "allocation failed; stopping")
            << "allocation " << at << " of " << total;
    }
}

// **The stop leaves what a crash leaves, and the mount restores it** - the
// premise the fail-stop rests on (`docs/rules/rules.md` §7, `sched.md` §3).
// A child builds a file-backed two-core volume (production's wiring, its
// reactors never started, so the statements run inline on this thread) and
// commits keys 1..20 `strict`. Session A opens a transaction and inserts 21;
// session B commits 22 `strict`, whose sync puts A's records in the log's
// file too. A's next INSERT, of 23, stops at an allocation (the counters are
// this thread's). The parent finds A's records durable and uncommitted in
// the log, mounts through production's mount - which must undo A over a
// leaf B's winner also wrote - and finds 1..20 and 22, neither of A's keys,
// and both free.
//
// The child is this test re-executed (the `threadsafe` style), so the volume's
// directory reaches it through the environment the parent set.
TEST_F(BadAllocDeathTest, AWriteStoppedAtAFailedAllocationIsRestoredByTheMount) {
    constexpr const char* kDirVar = "KDS_BAD_ALLOC_RECOVERY_DIR";
    if (!::testing::internal::InDeathTestChild()) {
        const std::filesystem::path made = std::filesystem::temp_directory_path() /
                                           ("kds_bad_alloc_recovery_" + std::to_string(::getpid()));
        ASSERT_EQ(::setenv(kDirVar, made.c_str(), /*overwrite=*/1), 0);
    }
    const char* named = std::getenv(kDirVar);
    ASSERT_NE(named, nullptr);
    const std::filesystem::path dir = named;
    struct RemoveAtExit {
        std::filesystem::path dir;
        ~RemoveAtExit() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            ::unsetenv("KDS_BAD_ALLOC_RECOVERY_DIR");
        }
    } remove_at_exit{dir};
    std::error_code cleared;
    std::filesystem::remove_all(dir, cleared);

    EXPECT_DEATH(
        {
            // Production's stack over `kds.db` and `wal/` in `dir`.
            TwoCoreRig::Options options;
            options.file_backed = true;
            options.dir = dir;
            auto rig = TwoCoreRig::Open(options);
            if (!rig.ok()) std::exit(2);
            CommandDispatcher& d = rig.value()->core(0).dispatcher();
            // `strict`: a commit is in the log's file before it answers.
            Session a;
            a.set_durability(wal::DurabilityClass::kStrict);
            Session b;
            b.set_durability(wal::DurabilityClass::kStrict);
            const auto run = [&](Session& s, const std::string& sql) {
                return d.Dispatch(sql, &s).response;
            };
            const auto must = [&](Session& s, const std::string& sql, const char* want, int code) {
                if (run(s, sql).rfind(want, 0) != 0) std::exit(code);
            };
            must(a, "CREATE TABLE t (id int64, k int64) BTREE", "CREATED", 3);
            for (int i = 1; i <= 20; ++i) {
                must(a, "INSERT INTO t VALUES (" + std::to_string(i) + ", 0)", "INSERTED", 4);
            }
            must(a, "BEGIN", "BEGIN", 5);
            // A's first INSERT, counted: its shape is the one that stops.
            std::size_t total = 0;
            {
                CountAllocations counting;
                must(a, "INSERT INTO t VALUES (21, 0)", "INSERTED", 6);
                total = counting.count();
            }
            if (total < 8) std::exit(7);
            must(b, "INSERT INTO t VALUES (22, 0)", "INSERTED", 8);
            base::InstallAllocationFailStop();
            FailNthAllocation fail(total / 2);
            run(a, "INSERT INTO t VALUES (23, 0)");
            std::exit(1);  // reaching here is the defect: the statement came back
        },
        "allocation failed; stopping");

    // Not vacuous: the log holds a transaction with an insert and no commit -
    // A's, made durable by B's sync. Read before the mount appends to it.
    const std::optional<crash_rig::Segment> seg = crash_rig::ReadSegment(dir / "wal");
    ASSERT_TRUE(seg.has_value());
    std::set<std::uint64_t> inserted;
    std::set<std::uint64_t> committed;
    for (std::size_t i = 0; i < seg->types.size(); ++i) {
        if (seg->types[i] == wal::RecordType::kBtreeInsert) inserted.insert(seg->txn_ids[i]);
        if (seg->types[i] == wal::RecordType::kTxnCommit) committed.insert(seg->txn_ids[i]);
    }
    std::size_t losers = 0;
    for (const std::uint64_t txn : inserted) losers += committed.count(txn) == 0 ? 1 : 0;
    ASSERT_EQ(losers, 1u) << "the stopped transaction's records never reached the log";

    auto mounted = crash_rig::Mount(dir);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the stop: " << mounted.status().message();
    CommandDispatcher& d = mounted.value()->dispatcher();
    std::vector<std::uint64_t> want;
    for (std::uint64_t i = 1; i <= 20; ++i) want.push_back(i);
    want.push_back(22);
    EXPECT_EQ(crash_rig::Ids(d, "SELECT id FROM t"), want)
        << "every committed row, and neither of the stopped transaction's";
    // A's keys are free: its insert was undone, not left delete-marked.
    for (const char* key : {"21", "23"}) {
        EXPECT_TRUE(crash_rig::StartsWith(
            d.Dispatch(std::string("INSERT INTO t VALUES (") + key + ", 0)").response, "INSERTED"))
            << "key " << key;
    }
}

TEST_F(BadAllocStatementTest, WithNoNewHandlerTheThrownBadAllocStillStopsAtTheCoroutine) {
    const std::string sql = "SELECT k, COUNT(*) FROM t GROUP BY k";
    const std::size_t total = AllocationsOf(sql);
    ASSERT_GT(total, 8u);

    // The backstop: a process that never installed the handler, or an
    // allocation that throws without reaching it.
    EXPECT_DEATH(
        {
            FailNthAllocation fail(total / 2);
            RunAsync(sql);
            std::exit(1);
        },
        "exception escaped a coroutine");
}

}  // namespace
}  // namespace kds::server
