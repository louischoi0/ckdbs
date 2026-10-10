// An allocation failure inside a statement must stop the process, not
// complete the statement as OK.
//
// The standard library throws `std::bad_alloc` from every container the
// engine grows, exceptions are enabled in the build, and nothing catches
// them. A statement on the default port runs inside a `sched::Coro`, and
// `Coro::promise_type::unhandled_exception()` used to do nothing: the throw
// unwound to the coroutine body, was swallowed, and the coroutine reached
// `final_suspend` as done with its result still the default `Status::OK()`
// and the statement's outcome never assigned. A statement that threw
// therefore answered as a success - an aggregated `SELECT` with no rows, a
// `COMMIT` that never ran reported as one that had.
//
// What a write leaves behind when interrupted between a page write and its
// record is not something to continue from, so the rule is one for every
// statement: **an exception that escapes a coroutine stops the process**, and
// the log restores what the stop interrupted. The cells run in death tests,
// since the process is not left usable.

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
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

void Drive(sched::Coro& c) {
    while (!c.done()) c.TryResumeDeepest();
}

TEST(BadAllocDeathTest, AnExceptionEscapingACoroutineStopsTheProcess) {
    EXPECT_DEATH(
        {
            sched::Coro c = ThrowsBadAlloc();
            Drive(c);
        },
        "exception escaped a coroutine");
}

class BadAllocStatementTest : public ::testing::Test {
protected:
    void SetUp() override {
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

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(BadAllocStatementTest, AnAggregatedSelectThatMeetsAFailedAllocationDoesNotAnswerOk) {
    const std::string sql = "SELECT k, COUNT(*) FROM t GROUP BY k";
    std::size_t total = 0;
    {
        CountAllocations counting;
        const DispatchOutcome whole = RunAsync(sql);
        total = counting.count();
        ASSERT_NE(whole.response.find("count(*)"), std::string::npos) << whole.response;
    }
    ASSERT_GT(total, 8u);

    // Inside the statement, past the coroutine's own frame, at three depths.
    for (const std::size_t at : {total / 4, total / 2, (total * 3) / 4}) {
        EXPECT_DEATH(
            {
                FailNthAllocation fail(at);
                const DispatchOutcome out = RunAsync(sql);
                // Reaching here is the defect: the statement came back, as a
                // success or an empty answer.
                std::exit(out.response.empty() ? 3 : 4);
            },
            "exception escaped a coroutine")
            << "allocation " << at << " of " << total;
    }
}

}  // namespace
}  // namespace kds::server
