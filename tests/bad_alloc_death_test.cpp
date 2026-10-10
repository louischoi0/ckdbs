// An allocation failure inside a statement stops the process; it does not
// complete the statement as OK (`docs/spec/sched.md` §3). The cells run in
// death tests, since the process is not left usable.

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
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
