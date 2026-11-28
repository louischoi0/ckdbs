// EG-S1: a `strict` commit on a one-core instance syncs on the statement's own
// stack - there is no wake registry, so nothing defers it and it never reaches
// the dispatcher's commit park, which is where `commit.wal_flush` was opened.
// Its sync is booked by `WalManager::Commit` instead, and this cell pins it
// from a statement down: one booked wait a `strict` commit through the
// dispatcher, and none added by a statement that commits nothing.
//
// The simulator's dispatcher is synchronous and no reactor has bound its
// thread to a core, so the wait is booked to the thread's own totals, which
// is where this cell reads it; that a core's totals are what `sys.wait_stats`
// shows, and that a scope opened inside a polled task also books to its
// session, are `wait_state_test.cpp`'s and `show_sugar_contract_test.cpp`'s.

#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "kds/sched/wait_state.hpp"
#include "kds/stats/wait_event.hpp"
#include "sim/instance.hpp"

namespace kds {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

std::uint64_t BookedCommitFlushes() {
    return sched::WaitTotals()
        .totals[static_cast<std::size_t>(stats::WaitEvent::kCommitWalFlush)]
        .count.get();
}

TEST(StrictCommitWaitTest, AOneCoreStrictCommitBooksItsSyncUnderCommitWalFlush) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    ASSERT_FALSE(IsErr(db.Execute("CREATE TABLE t (id int64, n int64)")));
    ASSERT_FALSE(IsErr(db.Execute("SET DURABILITY strict")));

    const std::uint64_t before = BookedCommitFlushes();
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (1, 10)")));
    EXPECT_EQ(BookedCommitFlushes(), before + 1);
    ASSERT_FALSE(IsErr(db.Execute("UPDATE t SET n = 11 WHERE id = 1")));
    EXPECT_EQ(BookedCommitFlushes(), before + 2);

    // A read commits nothing and waits on no sync.
    ASSERT_FALSE(IsErr(db.Execute("SELECT * FROM t WHERE id = 1")));
    EXPECT_EQ(BookedCommitFlushes(), before + 2);
}

}  // namespace
}  // namespace kds
