#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/sort.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The output sort - OB1-OB6 end to end (docs/workplan-order-by.md).
//
// The contract this file pins, and the reason most tests here compare a
// sorted reply against a *separately sorted* unsorted reply rather than
// against a literal:
//
//   **The reply to `ORDER BY k` is the unlimited reply, permuted into k's
//   order, with ties left in the order the chain emitted them.** A literal
//   expectation proves the engine agrees with whoever typed the literal; a
//   permutation check proves it agrees with the definition.
//
// And beside it, the claim that pays for the feature's memory (OB5): with
// a LIMIT, the sort holds `offset + limit` rows and not the relation -
// visible through ANALYZE's `sorted=` against its `examined=`.

namespace kds::server {
namespace {

class OrderByExecTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) {
        auto out = dispatcher_->Dispatch(sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), std::string::npos) << sql << ": " << out.response;
        return out.response;
    }

    std::string RunExpectingError(const std::string& sql) {
        return dispatcher_->Dispatch(sql).response;
    }

    // The reply's data rows, header dropped. Rows are separated by the
    // two-character escape the wire protocol uses, not by a newline.
    static std::vector<std::string> RowsOf(const std::string& reply) {
        std::vector<std::string> rows;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;) {
            const std::size_t start = at + 2;
            const std::size_t next = reply.find("\\n", start);
            rows.push_back(reply.substr(start, next == std::string::npos ? std::string::npos
                                                                         : next - start));
            at = next;
        }
        return rows;
    }

    std::vector<std::string> Rows(const std::string& sql) { return RowsOf(Run(sql)); }

    static std::uint64_t MeterOf(const std::string& reply, const std::string& key) {
        const auto at = reply.find(key + "=");
        EXPECT_NE(at, std::string::npos) << key << " not in: " << reply;
        if (at == std::string::npos) return 0;
        return std::strtoull(reply.c_str() + at + key.size() + 1, nullptr, 10);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- 1. The definition: a sorted reply is a permutation into key order ----

TEST_F(OrderByExecTest, ASortedReplyIsTheUnsortedReplyPermuted) {
    Run("CREATE TABLE t (id int64, v varchar)");
    for (const char* v : {"pear", "apple", "fig", "banana", "cherry"}) {
        Run(std::string("INSERT INTO t VALUES ('") + v + "')");
    }

    std::vector<std::string> expected = Rows("SELECT v FROM t");
    ASSERT_EQ(expected.size(), 5u);
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v"), expected);

    std::reverse(expected.begin(), expected.end());
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v DESC"), expected);
}

TEST_F(OrderByExecTest, TheSameOnABtreeRelation) {
    Run("CREATE TABLE t (id int64, v varchar) BTREE");
    for (const char* v : {"pear", "apple", "fig"}) {
        Run(std::string("INSERT INTO t VALUES ('") + v + "')");
    }
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v"),
              (std::vector<std::string>{"apple", "fig", "pear"}));
}

// ---- 2. Every type this engine can order ----------------------------------
//
// One case per arm of `OrderKeyOf`, because each arm is a separate way to
// get the order wrong and the failures do not look alike: a uint64 above
// INT64_MAX sorts below zero if the signed reading wins, a DATE sorts as a
// string if the epoch integer is rendered before it is compared, and a
// wide decimal loses its high half.

TEST_F(OrderByExecTest, Uint64OrdersAboveInt64Max) {
    Run("CREATE TABLE t (id int64, u uint64)");
    Run("INSERT INTO t VALUES (5)");
    Run("INSERT INTO t VALUES (18446744073709551615)");
    Run("INSERT INTO t VALUES (9223372036854775808)");
    // Signed int64 would order the two large values *below* 5.
    EXPECT_EQ(Rows("SELECT u FROM t ORDER BY u"),
              (std::vector<std::string>{"5", "9223372036854775808", "18446744073709551615"}));
}

TEST_F(OrderByExecTest, DecimalOrdersByValueAndRendersAtItsScale) {
    Run("CREATE TABLE t (id int64, amt decimal(12, 2))");
    for (const char* v : {"10.05", "2.50", "100.00", "-3.75"}) {
        Run(std::string("INSERT INTO t VALUES (") + v + ")");
    }
    EXPECT_EQ(Rows("SELECT amt FROM t ORDER BY amt"),
              (std::vector<std::string>{"-3.75", "2.50", "10.05", "100.00"}));
}

TEST_F(OrderByExecTest, WideDecimalOrdersAcrossItsHighHalf) {
    Run("CREATE TABLE t (id int64, amt decimal(30, 2))");
    // Above 2^64 unscaled, so the value lives in the Int128's high half -
    // which a comparator reading only the low half would order wrongly.
    for (const char* v : {"1.00", "1701411834604692317316.87", "-5.00"}) {
        Run(std::string("INSERT INTO t VALUES ('") + v + "')");
    }
    EXPECT_EQ(Rows("SELECT amt FROM t ORDER BY amt"),
              (std::vector<std::string>{"-5.00", "1.00", "1701411834604692317316.87"}));
}

TEST_F(OrderByExecTest, DateAndTimestampOrderChronologically) {
    Run("CREATE TABLE t (id int64, d date, ts timestamp)");
    Run("INSERT INTO t VALUES ('2026-08-11', '2026-08-11 10:00:00')");
    Run("INSERT INTO t VALUES ('1999-01-01', '1999-01-01 00:00:00')");
    Run("INSERT INTO t VALUES ('2026-01-01', '2026-01-01 23:59:59')");
    EXPECT_EQ(Rows("SELECT d FROM t ORDER BY d"),
              (std::vector<std::string>{"1999-01-01", "2026-01-01", "2026-08-11"}));
    EXPECT_EQ(Rows("SELECT d FROM t ORDER BY ts DESC"),
              (std::vector<std::string>{"2026-08-11", "2026-01-01", "1999-01-01"}));
}

// Two strings sharing a 32-byte prefix. An index key truncates at
// `kIndexStringKeyBytes` and would call these equal, which is one of the
// reasons an index cannot serve an order (sort.hpp) - the sort compares the
// whole value.
TEST_F(OrderByExecTest, StringsSharingALongPrefixStillOrder) {
    Run("CREATE TABLE t (id int64, v varchar)");
    const std::string prefix(32, 'x');
    Run("INSERT INTO t VALUES ('" + prefix + "b')");
    Run("INSERT INTO t VALUES ('" + prefix + "a')");
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v"),
              (std::vector<std::string>{prefix + "a", prefix + "b"}));
}

// ---- 3. Multi-key, mixed direction, and stability -------------------------

TEST_F(OrderByExecTest, LaterKeysBreakEarlierTies) {
    Run("CREATE TABLE t (id int64, grp varchar, v varchar)");
    Run("INSERT INTO t VALUES ('b', 'two')");
    Run("INSERT INTO t VALUES ('a', 'two')");
    Run("INSERT INTO t VALUES ('b', 'one')");
    Run("INSERT INTO t VALUES ('a', 'one')");
    EXPECT_EQ(Rows("SELECT grp, v FROM t ORDER BY grp, v"),
              (std::vector<std::string>{"a,one", "a,two", "b,one", "b,two"}));
    // Each key keeps its own direction: descending on grp, ascending on v.
    EXPECT_EQ(Rows("SELECT grp, v FROM t ORDER BY grp DESC, v"),
              (std::vector<std::string>{"b,one", "b,two", "a,one", "a,two"}));
}

// Rows the clause does not distinguish come back in the order the chain
// emitted them - so `ORDER BY` *refines* I12's emission contract rather
// than replacing it. `seq` as the last sort key is what makes this a
// property of the comparator rather than of the algorithm.
TEST_F(OrderByExecTest, TiesKeepChainEmissionOrder) {
    Run("CREATE TABLE t (id int64, grp varchar, v varchar)");
    for (const char* v : {"first", "second", "third", "fourth"}) {
        Run(std::string("INSERT INTO t VALUES ('same', '") + v + "')");
    }
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY grp"),
              (std::vector<std::string>{"first", "second", "third", "fourth"}));
    // ...and descending on the tied key does not reverse the ties either:
    // the direction belongs to `grp`, and `seq` is always ascending.
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY grp DESC"),
              (std::vector<std::string>{"first", "second", "third", "fourth"}));
}

// ---- 4. Ordering by something the client never sees ------------------------

TEST_F(OrderByExecTest, OrderByANonProjectedColumn) {
    Run("CREATE TABLE t (id int64, k int64, v varchar)");
    Run("INSERT INTO t VALUES (30, 'c')");
    Run("INSERT INTO t VALUES (10, 'a')");
    Run("INSERT INTO t VALUES (20, 'b')");
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY k"),
              (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(OrderByExecTest, OrderByAJoinedRelationsColumn) {
    Run("CREATE TABLE acct (id int64, name varchar)");
    Run("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    Run("INSERT INTO acct VALUES ('ann')");
    Run("INSERT INTO acct VALUES ('bob')");
    Run("INSERT INTO trade VALUES (1, 'ZZZ')");
    Run("INSERT INTO trade VALUES (2, 'AAA')");
    Run("INSERT INTO trade VALUES (1, 'MMM')");
    EXPECT_EQ(Rows("SELECT t.sym FROM acct AS a JOIN trade AS t ON t.acct_id = a.id "
                   "ORDER BY t.sym"),
              (std::vector<std::string>{"AAA", "MMM", "ZZZ"}));
}

// ---- 5. Composition with the quota (OB4) and the top-N heap (OB5) ---------

// The defining contract, restated for a sorted statement: the slice is of
// the *sorted* reply. This is the one thing the sort had to move the quota
// downstream to keep true.
TEST_F(OrderByExecTest, TheLimitSlicesTheSortedReplyNotTheEmittedOne) {
    Run("CREATE TABLE t (id int64, v varchar)");
    for (const char* v : {"pear", "apple", "fig", "banana", "cherry"}) {
        Run(std::string("INSERT INTO t VALUES ('") + v + "')");
    }
    const std::vector<std::string> all = Rows("SELECT v FROM t ORDER BY v");
    ASSERT_EQ(all, (std::vector<std::string>{"apple", "banana", "cherry", "fig", "pear"}));

    for (std::size_t offset = 0; offset <= 5; ++offset) {
        for (std::size_t limit = 0; limit <= 6; ++limit) {
            const std::vector<std::string> want(
                all.begin() + static_cast<long>(std::min(offset, all.size())),
                all.begin() + static_cast<long>(std::min(offset + limit, all.size())));
            EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v LIMIT " + std::to_string(limit) +
                           " OFFSET " + std::to_string(offset)),
                      want)
                << "limit=" << limit << " offset=" << offset;
        }
    }
}

// Stability has to survive the top-N heap, where the losing row is
// discarded before it is ever rendered rather than sorted and dropped.
// A later row tying the heap's worst on every written key must not
// displace it: the retained row arrived first, and arrival order is the
// last key. Every row here ties, so the answer is the first `limit` in
// emission order and any other answer means the discard rule inverted.
TEST_F(OrderByExecTest, TiesDoNotDisplaceARetainedRowUnderALimit) {
    Run("CREATE TABLE t (id int64, grp varchar, v varchar)");
    for (const char* v : {"first", "second", "third", "fourth", "fifth"}) {
        Run(std::string("INSERT INTO t VALUES ('same', '") + v + "')");
    }
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY grp LIMIT 2"),
              (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY grp DESC LIMIT 2"),
              (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY grp LIMIT 2 OFFSET 2"),
              (std::vector<std::string>{"third", "fourth"}));
}

// The heap evicts, and what it evicts must be gone from the answer -
// including when the winning rows arrive last, which is the order that
// exercises every eviction.
TEST_F(OrderByExecTest, EveryRetainedRowCanBeDisplacedInTurn) {
    Run("CREATE TABLE t (id int64, k int64)");
    for (int k = 20; k >= 1; --k) Run("INSERT INTO t VALUES (" + std::to_string(k) + ")");
    EXPECT_EQ(Rows("SELECT k FROM t ORDER BY k LIMIT 3"),
              (std::vector<std::string>{"1", "2", "3"}));
    // ...and ascending arrival, where nothing after the first three ever
    // beats the heap and every later row is discarded unrendered.
    Run("CREATE TABLE u (id int64, k int64)");
    for (int k = 1; k <= 20; ++k) Run("INSERT INTO u VALUES (" + std::to_string(k) + ")");
    EXPECT_EQ(Rows("SELECT k FROM u ORDER BY k LIMIT 3"),
              (std::vector<std::string>{"1", "2", "3"}));
}

TEST_F(OrderByExecTest, ADescendingLimitTakesTheLargestNotTheLast) {
    Run("CREATE TABLE t (id int64, k int64)");
    for (int k : {3, 1, 5, 2, 4}) Run("INSERT INTO t VALUES (" + std::to_string(k) + ")");
    EXPECT_EQ(Rows("SELECT k FROM t ORDER BY k DESC LIMIT 2"),
              (std::vector<std::string>{"5", "4"}));
}

// OB5's claim, in the only place it leaves a trace: the sort held
// `offset + limit` rows, not the relation. `examined=` is what the walk
// cost and does not shrink - a sorted statement cannot stop early.
TEST_F(OrderByExecTest, TopNHoldsOffsetPlusLimitRowsNotTheRelation) {
    Run("CREATE TABLE t (id int64, k int64)");
    for (int i = 0; i < 60; ++i) Run("INSERT INTO t VALUES (" + std::to_string(60 - i) + ")");

    const std::string analyzed = Run("ANALYZE SELECT k FROM t ORDER BY k LIMIT 3 OFFSET 2");
    EXPECT_EQ(MeterOf(analyzed, "sorted"), 5u);
    EXPECT_EQ(MeterOf(analyzed, "examined"), 60u);
    EXPECT_EQ(MeterOf(analyzed, "rows"), 3u);

    // ...and the answer is still right, which is the half a memory bound
    // is easy to get at the expense of.
    EXPECT_EQ(Rows("SELECT k FROM t ORDER BY k LIMIT 3 OFFSET 2"),
              (std::vector<std::string>{"3", "4", "5"}));

    const std::string unlimited = Run("ANALYZE SELECT k FROM t ORDER BY k");
    EXPECT_EQ(MeterOf(unlimited, "sorted"), 60u);
}

// ---- 6. The elision: the free order stays free ----------------------------

TEST_F(OrderByExecTest, OrderByThePkBuildsNoSortAndPrintsNoSortLine) {
    Run("CREATE TABLE t (id int64, v varchar)");
    for (const char* v : {"a", "b", "c"}) {
        Run(std::string("INSERT INTO t VALUES ('") + v + "')");
    }
    const std::string plan = Run("ANALYZE SELECT v FROM t ORDER BY id");
    EXPECT_EQ(plan.find("sort "), std::string::npos) << plan;
    EXPECT_EQ(plan.find("sorted="), std::string::npos) << plan;

    // ...and a LIMIT on the elided path still stops the walk, which is the
    // property the elision exists to preserve.
    const std::string limited = Run("ANALYZE SELECT v FROM t ORDER BY id LIMIT 1");
    EXPECT_EQ(MeterOf(limited, "examined"), 1u);
}

TEST_F(OrderByExecTest, ASortedPlanNamesItsKeysAndTheirDirections) {
    Run("CREATE TABLE t (id int64, a varchar, b varchar)");
    Run("INSERT INTO t VALUES ('x', 'y')");
    const std::string plan = Run("ANALYZE SELECT a FROM t ORDER BY b DESC, a");
    EXPECT_NE(plan.find("sort "), std::string::npos) << plan;
    EXPECT_NE(plan.find("desc"), std::string::npos) << plan;
    EXPECT_NE(plan.find("asc"), std::string::npos) << plan;
}

// ---- 7. The cap refuses, and names the key --------------------------------

TEST_F(OrderByExecTest, TheRowCapRefusesAndNamesTheConfigKey) {
    dispatcher_->set_sort_max_rows(3);
    Run("CREATE TABLE t (id int64, v varchar)");
    for (int i = 0; i < 5; ++i) Run("INSERT INTO t VALUES ('v')");

    const std::string reply = RunExpectingError("SELECT v FROM t ORDER BY v");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("sort_max_rows"), std::string::npos) << reply;

    // A LIMIT within the cap succeeds over the same relation: what the cap
    // bounds is rows *held*, and top-N holds four.
    dispatcher_->set_sort_max_rows(4);
    EXPECT_EQ(Rows("SELECT v FROM t ORDER BY v LIMIT 4").size(), 4u);
}

// A cap of zero refuses even under a LIMIT, where the top-N heap would
// otherwise have nothing to retain and would hand back the empty reply -
// a truncation wearing a legal statement's shape. `LIMIT 0` is the legal
// statement it must not be confused with, and it still answers no rows.
TEST_F(OrderByExecTest, ACapOfZeroRefusesRatherThanAnsweringNothing) {
    dispatcher_->set_sort_max_rows(0);
    Run("CREATE TABLE t (id int64, v varchar)");
    for (int i = 0; i < 3; ++i) Run("INSERT INTO t VALUES ('v')");

    EXPECT_EQ(RunExpectingError("SELECT v FROM t ORDER BY v").rfind("ERR", 0), 0u);
    EXPECT_EQ(RunExpectingError("SELECT v FROM t ORDER BY v LIMIT 2").rfind("ERR", 0), 0u);
    // `LIMIT 0` asks for nothing and gets nothing, cap or no cap.
    EXPECT_EQ(Run("SELECT v FROM t ORDER BY v LIMIT 0"), "v");
}

// A cap refuses; it never truncates. A three-row answer to a five-row
// question wears a right answer's shape, which is the whole reason.
TEST_F(OrderByExecTest, TheCapEmitsNoPartialAnswer) {
    dispatcher_->set_sort_max_rows(2);
    Run("CREATE TABLE t (id int64, v varchar)");
    for (int i = 0; i < 4; ++i) Run("INSERT INTO t VALUES ('v')");
    const std::string reply = RunExpectingError("SELECT v FROM t ORDER BY v");
    EXPECT_EQ(reply.find("\\n"), std::string::npos) << reply;
}

// A `LIMIT` above the cap does not make the cap stop refusing. The
// retention target is `offset + limit`, so a limit the cap cannot cover
// leaves the sort holding the whole qualifying set - the unlimited case
// under another name - and clamping the target to the cap instead would
// answer with the first `sort_max_rows` rows of the order and no error,
// which is exactly the truncation the cap exists to prevent.
TEST_F(OrderByExecTest, ALimitAboveTheCapStillRefusesWhenThatManyRowsArrive) {
    dispatcher_->set_sort_max_rows(3);
    Run("CREATE TABLE t (id int64, v varchar)");
    for (int i = 0; i < 5; ++i) Run("INSERT INTO t VALUES ('v" + std::to_string(i) + "')");

    const std::string reply = RunExpectingError("SELECT v FROM t ORDER BY v LIMIT 100");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("sort_max_rows"), std::string::npos) << reply;

    // ...and the same statement over a set the cap does cover still
    // succeeds: the cap fires on rows that arrive, never on the limit.
    Run("CREATE TABLE small (id int64, v varchar)");
    for (int i = 0; i < 3; ++i) Run("INSERT INTO small VALUES ('v" + std::to_string(i) + "')");
    EXPECT_EQ(Rows("SELECT v FROM small ORDER BY v LIMIT 100").size(), 3u);
}

// `sort_max_rows = 0` refuses every statement that needs a sort, limited
// or not - the setting's documented meaning (expeditor.hpp). Only `LIMIT 0`
// escapes, because it needs no row held.
TEST_F(OrderByExecTest, AZeroCapRefusesALimitedSortToo) {
    dispatcher_->set_sort_max_rows(0);
    Run("CREATE TABLE t (id int64, v varchar)");
    for (int i = 0; i < 2; ++i) Run("INSERT INTO t VALUES ('v')");
    EXPECT_EQ(RunExpectingError("SELECT v FROM t ORDER BY v LIMIT 1").rfind("ERR", 0), 0u);
    EXPECT_TRUE(Rows("SELECT v FROM t ORDER BY v LIMIT 0").empty());
}

// ---- 8. What stays refused ------------------------------------------------

TEST_F(OrderByExecTest, OrderByOverAnAggregatedStatementIsStillRefused) {
    Run("CREATE TABLE t (id int64, grp varchar)");
    Run("INSERT INTO t VALUES ('a')");
    const std::string reply = RunExpectingError("SELECT grp, COUNT(*) FROM t GROUP BY grp "
                                                "ORDER BY grp");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("ORDER BY"), std::string::npos) << reply;
}

// ---- 9. OrderKey: the ordering decision, unit-tested -----------------------

parser::AstValue Str(std::string v) {
    parser::AstValue out;
    out.type = parser::ValueType::kStr;
    out.str_val = std::move(v);
    return out;
}

TEST(OrderKeyTest, StringOrderIsByteOrderOverTheWholeValue) {
    const std::string prefix(40, 'q');
    auto a = exec::OrderKeyOf(0, Str(prefix + "a"));
    auto b = exec::OrderKeyOf(0, Str(prefix + "b"));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_LT(a.value().Compare(b.value()), 0);
    EXPECT_GT(b.value().Compare(a.value()), 0);
    EXPECT_EQ(a.value().Compare(a.value()), 0);
}

// D3 (docs/spec/null.md): a NULL orders above every value - ASC puts it
// last and the ordinary descending flip puts it first - and two NULLs tie.
TEST(OrderKeyTest, ANullOrdersAboveEveryValueAndTiesWithItself) {
    parser::AstValue null_value;
    null_value.type = parser::ValueType::kNull;
    auto null_key = exec::OrderKeyOf(0, null_value);
    ASSERT_TRUE(null_key.ok()) << null_key.status().message();

    parser::AstValue big;
    big.type = parser::ValueType::kInt;
    big.int_val = std::numeric_limits<std::int64_t>::max();
    auto big_key = exec::OrderKeyOf(0, big);
    ASSERT_TRUE(big_key.ok());
    EXPECT_GT(null_key.value().Compare(big_key.value()), 0);
    EXPECT_LT(big_key.value().Compare(null_key.value()), 0);

    auto str_key = exec::OrderKeyOf(0, Str("zzz"));
    ASSERT_TRUE(str_key.ok());
    EXPECT_GT(null_key.value().Compare(str_key.value()), 0);

    EXPECT_EQ(null_key.value().Compare(null_key.value()), 0);
}

// ---- 10. NULL under ORDER BY, end to end (null.md D3, NU7) ------------

TEST_F(OrderByExecTest, NullsSortLastAscendingAndFirstDescending) {
    Run("CREATE TABLE t (id int64, n int64 NULL)");
    for (const char* v : {"3", "NULL", "1", "NULL", "2"}) {
        Run(std::string("INSERT INTO t VALUES (") + v + ")");
    }
    EXPECT_EQ(Rows("SELECT n FROM t ORDER BY n"),
              (std::vector<std::string>{"1", "2", "3", "NULL", "NULL"}));
    EXPECT_EQ(Rows("SELECT n FROM t ORDER BY n DESC"),
              (std::vector<std::string>{"NULL", "NULL", "3", "2", "1"}));
}

// The top-N heap (OB5) places NULLs by the same order: an ascending LIMIT
// never surfaces a NULL while enough real values exist, and a descending
// one surfaces exactly the NULLs.
TEST_F(OrderByExecTest, TheTopNHeapKeepsD3sNullPosition) {
    Run("CREATE TABLE t (id int64, n int64 NULL)");
    for (const char* v : {"3", "NULL", "1", "NULL", "2"}) {
        Run(std::string("INSERT INTO t VALUES (") + v + ")");
    }
    EXPECT_EQ(Rows("SELECT n FROM t ORDER BY n LIMIT 3"),
              (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_EQ(Rows("SELECT n FROM t ORDER BY n DESC LIMIT 2"),
              (std::vector<std::string>{"NULL", "NULL"}));
}

// A NULL in the second key orders within its group, and ties between two
// NULLs keep the chain's emission order (seq is the final key).
TEST_F(OrderByExecTest, ANullSecondaryKeyOrdersWithinItsGroup) {
    Run("CREATE TABLE t (id int64, grp int64, n int64 NULL)");
    Run("INSERT INTO t VALUES (1, NULL)");
    Run("INSERT INTO t VALUES (1, 5)");
    Run("INSERT INTO t VALUES (2, NULL)");
    Run("INSERT INTO t VALUES (2, 4)");
    EXPECT_EQ(Rows("SELECT grp, n FROM t ORDER BY grp, n"),
              (std::vector<std::string>{"1,5", "1,NULL", "2,4", "2,NULL"}));
    EXPECT_EQ(Rows("SELECT grp, n FROM t ORDER BY grp, n DESC"),
              (std::vector<std::string>{"1,NULL", "1,5", "2,NULL", "2,4"}));
}

}  // namespace
}  // namespace kds::server
