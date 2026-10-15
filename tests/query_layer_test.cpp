#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The query layer (BU-S4..S7, instructions/v3.0.0/workorder-bu-basic-query-surface.md
// BU-Q8..Q12): every SELECT form a step chain cannot carry, answered over
// native walks. The dataset is anti-kdbs F004's, and each expected answer is
// the one PostgreSQL gives (BU-R1).

namespace kds::server {
namespace {

class QueryLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        cabins_.emplace();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), &*recorder_, /*replay=*/true,
                            /*access_statistics=*/true, &*cabins_);

        Ok("CREATE TABLE cust (id int64, name varchar(32), region varchar(16), tier int64 NULL)");
        Ok("INSERT INTO cust VALUES (1, 'alice', 'EU', 1)");
        Ok("INSERT INTO cust VALUES (2, 'bob', 'US', 2)");
        Ok("INSERT INTO cust VALUES (3, 'carol', 'EU', NULL)");
        Ok("INSERT INTO cust VALUES (4, 'dave', 'APAC', 3)");
        Ok("INSERT INTO cust VALUES (5, 'erin', 'US', NULL)");
        Ok("INSERT INTO cust VALUES (6, 'frank', 'EU', 2)");
        Ok("CREATE TABLE ord (id int64, cust_id int64, status varchar(16), amount int64, "
           "placed date, note varchar(32) NULL)");
        Ok("INSERT INTO ord VALUES (10, 1, 'open', 100, '2026-01-01', NULL)");
        Ok("INSERT INTO ord VALUES (11, 1, 'paid', 250, '2026-01-02', 'rush')");
        Ok("INSERT INTO ord VALUES (12, 2, 'paid', 75, '2026-01-02', NULL)");
        Ok("INSERT INTO ord VALUES (13, 2, 'cancel', 300, '2026-01-03', 'dup')");
        Ok("INSERT INTO ord VALUES (14, 4, 'open', 50, '2026-01-04', NULL)");
        Ok("INSERT INTO ord VALUES (15, 4, 'paid', 500, '2026-01-05', 'vip')");
        Ok("INSERT INTO ord VALUES (16, 6, 'open', 120, '2026-01-05', NULL)");
        Ok("INSERT INTO ord VALUES (17, 99, 'open', 10, '2026-01-06', 'orphan')");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    // The reply's header and rows; `sorted` puts the rows in byte order, for
    // a statement whose order SQL does not fix.
    static std::vector<std::string> Lines(const std::string& reply, bool sorted) {
        std::vector<std::string> out;
        std::size_t at = 0;
        for (;;) {
            const std::size_t next = reply.find("\\n", at);
            out.push_back(reply.substr(at, next == std::string::npos ? std::string::npos : next - at));
            if (next == std::string::npos) break;
            at = next + 2;
        }
        if (sorted && out.size() > 1) std::sort(out.begin() + 1, out.end());
        return out;
    }
    void ExpectRows(const std::string& sql, std::vector<std::string> want, bool ordered = false) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        if (!ordered && want.size() > 1) std::sort(want.begin() + 1, want.end());
        EXPECT_EQ(Lines(reply, !ordered), want) << sql;
    }
    void ExpectRefused(const std::string& sql, const std::string& needle) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(needle), std::string::npos) << sql << " -> " << reply;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- BU-S4: the select list ------------------------------------------------

TEST_F(QueryLayerTest, AColumnAliasNamesTheColumn) {
    ExpectRows("SELECT name AS who FROM cust",
               {"who", "alice", "bob", "carol", "dave", "erin", "frank"});
}

TEST_F(QueryLayerTest, ArithmeticLiteralAndConcatenationInTheSelectList) {
    ExpectRows("SELECT id, amount * 2 FROM ord",
               {"id,amount * 2", "10,200", "11,500", "12,150", "13,600", "14,100", "15,1000",
                "16,240", "17,20"});
    ExpectRows("SELECT id, 7 FROM cust",
               {"id,7", "1,7", "2,7", "3,7", "4,7", "5,7", "6,7"});
    ExpectRows("SELECT name || '@' || region FROM cust",
               {"name || '@' || region", "alice@EU", "bob@US", "carol@EU", "dave@APAC", "erin@US", "frank@EU"});
}

TEST_F(QueryLayerTest, CaseCoalesceCastAndAFunctionInTheSelectList) {
    ExpectRows("SELECT id, CASE WHEN amount >= 250 THEN 'big' ELSE 'small' END FROM ord",
               {"id,CASE WHEN amount >= 250 THEN 'big' ELSE 'small' END", "10,small", "11,big", "12,small", "13,big", "14,small", "15,big",
                "16,small", "17,small"});
    ExpectRows("SELECT id, COALESCE(tier, 0) FROM cust",
               {"id,COALESCE(tier, 0)", "1,1", "2,2", "3,0", "4,3", "5,0", "6,2"});
    ExpectRows("SELECT CAST(amount AS varchar) FROM ord",
               {"CAST(amount AS varchar)", "100", "250", "75", "300", "50", "500", "120", "10"});
    ExpectRows("SELECT UPPER(name) FROM cust",
               {"UPPER(name)", "ALICE", "BOB", "CAROL", "DAVE", "ERIN", "FRANK"});
}

TEST_F(QueryLayerTest, DistinctKeepsOneRowPerValue) {
    ExpectRows("SELECT DISTINCT region FROM cust", {"region", "APAC", "EU", "US"});
    ExpectRows("SELECT DISTINCT cust_id, status FROM ord",
               {"cust_id,status", "1,open", "1,paid", "2,cancel", "2,paid", "4,open", "4,paid",
                "6,open", "99,open"});
    // NULLs are one value to DISTINCT.
    ExpectRows("SELECT DISTINCT tier FROM cust", {"tier", "1", "2", "3", "NULL"});
}

TEST_F(QueryLayerTest, AQualifiedStarIsThatRelationsColumns) {
    ExpectRows("SELECT c.* FROM cust AS c",
               {"id,name,region,tier", "1,alice,EU,1", "2,bob,US,2", "3,carol,EU,NULL",
                "4,dave,APAC,3", "5,erin,US,NULL", "6,frank,EU,2"});
}

TEST_F(QueryLayerTest, OrderByAnExpressionAnAliasOrAPosition) {
    ExpectRows("SELECT id FROM ord ORDER BY amount * -1, id",
               {"id", "15", "13", "11", "16", "10", "12", "14", "17"}, /*ordered=*/true);
    ExpectRows("SELECT id, amount AS a FROM ord ORDER BY a, id",
               {"id,a", "17,10", "14,50", "12,75", "10,100", "16,120", "11,250", "13,300",
                "15,500"},
               /*ordered=*/true);
    ExpectRows("SELECT id, amount FROM ord ORDER BY 2 DESC, 1 LIMIT 3",
               {"id,amount", "15,500", "13,300", "11,250"}, /*ordered=*/true);
    // NULLs sort largest (null.md D3).
    ExpectRows("SELECT id, tier FROM cust ORDER BY tier DESC, id",
               {"id,tier", "3,NULL", "5,NULL", "4,3", "2,2", "6,2", "1,1"}, /*ordered=*/true);
}

TEST_F(QueryLayerTest, SelectWithoutFromIsOneRow) {
    ExpectRows("SELECT 1 + 1", {"1 + 1", "2"});
    ExpectRows("SELECT 'a' || 'b' AS ab, 3 * 4", {"ab,3 * 4", "ab,12"});
}

TEST_F(QueryLayerTest, ExpressionOperandsInWhere) {
    ExpectRows("SELECT id FROM ord WHERE amount * 2 > 400", {"id", "11", "13", "15"});
    ExpectRows("SELECT id FROM cust WHERE UPPER(name) = 'BOB'", {"id", "2"});
    // A walked term and a layer term together: the pk window is walked, the
    // expression evaluated over what the walk returned.
    ExpectRows("SELECT id, amount + 1 FROM ord WHERE id >= 12 AND amount % 100 = 0",
               {"id,amount + 1", "13,301", "15,501"});
}

TEST_F(QueryLayerTest, AnExpressionOverAJoin) {
    ExpectRows("SELECT o.id, c.name || '!' AS who FROM ord AS o JOIN cust AS c ON o.cust_id = c.id "
               "WHERE o.amount >= 250",
               {"o.id,who", "11,alice!", "13,bob!", "15,dave!"});
}

TEST_F(QueryLayerTest, ThreeValuedLogicInTheSelectList) {
    // A comparison is a bool value, unknown where an operand is NULL.
    ExpectRows("SELECT id, tier > 1 FROM cust",
               {"id,tier > 1", "1,0", "2,1", "3,NULL", "4,1", "5,NULL", "6,1"});
}

TEST_F(QueryLayerTest, InOverAnEmptySubqueryIsFalseEvenForANullOperand) {
    // `NULL IN (no rows)` is false, so `NULL NOT IN (no rows)` keeps the row
    // (BU-R4); carol and erin have a NULL tier.
    ExpectRows("SELECT id + 0 FROM cust WHERE tier NOT IN (SELECT tier FROM cust WHERE id > 100)",
               {"id + 0", "1", "2", "3", "4", "5", "6"});
    ExpectRows("SELECT id + 0 FROM cust WHERE tier IN (SELECT tier FROM cust WHERE id > 100)",
               {"id + 0"});
    // Over a non-empty set a NULL operand stays unknown.
    ExpectRows("SELECT id + 0 FROM cust WHERE tier NOT IN (SELECT tier FROM cust WHERE id = 1)",
               {"id + 0", "2", "4", "6"});
}

TEST_F(QueryLayerTest, OrderByANameTwoItemsReadAsOneColumnIsNotAmbiguous) {
    ExpectRows("SELECT *, name FROM cust ORDER BY name LIMIT 2",
               {"id,name,region,tier,name", "1,alice,EU,1,alice", "2,bob,US,2,bob"},
               /*ordered=*/true);
    ExpectRefused("SELECT name, region AS name FROM cust ORDER BY name", "is ambiguous");
}

TEST_F(QueryLayerTest, ARelationWalkIsBoundedBySortMaxRowsAsItRuns) {
    dispatcher_->set_sort_max_rows(5);
    ExpectRefused("SELECT id + 0 FROM ord", "sort_max_rows (5)");
    ExpectRows("SELECT id + 0 FROM ord WHERE id < 13", {"id + 0", "10", "11", "12"});
}

TEST_F(QueryLayerTest, AggregateMaxGroupsOfZeroRefusesAsTheNativeFoldDoes) {
    exec::AggregateLimits limits;
    limits.max_groups = 0;
    dispatcher_->set_aggregate_limits(limits);
    ExpectRefused("SELECT region, count(*) + 0 FROM cust GROUP BY region", "aggregate_max_groups");
}

// ---- Refusals: typed at compile, positioned ---------------------------------

TEST_F(QueryLayerTest, AMistypedExpressionIsRefusedWhetherOrNotARowMatches) {
    ExpectRefused("SELECT name + 1 FROM cust WHERE id = 999",
                  "integer literal cannot be used as varchar (byte 14)");
    ExpectRefused("SELECT id FROM cust WHERE name * 2 > 1",
                  "integer literal cannot be used as varchar (byte 33)");
    // A CAST to varchar(N) stays refused: N is a cell width, not a length
    // (BU-Q13), so no truncation PostgreSQL would apply can be meant.
    ExpectRefused("SELECT CAST(amount AS varchar(16)) FROM ord", "write varchar (byte 22)");
}

TEST_F(QueryLayerTest, AnUnknownOrAmbiguousColumnIsRefusedWithItsByte) {
    ExpectRefused("SELECT nope + 1 FROM cust", "unknown column 'nope' (byte 7)");
    ExpectRefused("SELECT id + 1 FROM cust AS a JOIN cust AS b ON a.id = b.id", "ambiguous");
}

TEST_F(QueryLayerTest, OrderByRefusals) {
    ExpectRefused("SELECT id FROM ord ORDER BY 2", "ORDER BY position 2 is not in the select list");
    ExpectRefused("SELECT DISTINCT status FROM ord ORDER BY amount",
                  "for SELECT DISTINCT, an ORDER BY expression must appear in the select list");
}

TEST_F(QueryLayerTest, WhatTheQueryGrammarRefusesKeepsTheNativeRefusal) {
    // `<>` stays refused by decision (BU-Q5), so neither grammar reads it.
    const std::string reply = Run("SELECT id + 1 FROM cust WHERE id <> 1");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    ExpectRefused("WITH x AS (SELECT id FROM cust) SELECT id FROM x", "ERR");
}

TEST_F(QueryLayerTest, ANativeStatementAnswersAsBefore) {
    ExpectRows("SELECT id FROM ord WHERE status = 'open' AND amount > 60", {"id", "10", "16"});
    ExpectRows("SELECT o.id, c.name FROM ord AS o JOIN cust AS c ON o.cust_id = c.id WHERE o.id = 12",
               {"o.id,c.name", "12,bob"});
}

// A qualifier and a `rel.*` match their binding ignoring case, as the
// native path does, and as a column name does since 2026-10-15.
TEST_F(QueryLayerTest, AQualifierMatchesItsBindingIgnoringCase) {
    ExpectRows("SELECT C.id + 0 FROM cust AS c WHERE id < 3", {"C.id + 0", "1", "2"});
    ExpectRows("SELECT C.* FROM cust AS c WHERE id = 1", {"id,name,region,tier", "1,alice,EU,1"});
    ExpectRefused("SELECT 1 FROM cust AS x JOIN ord AS X ON x.id = X.cust_id", "bound twice");
}

// What a later stage builds keeps the native reply until that stage lands
// (`exec::FormsLanded`): the layer reads these and does not answer them.
TEST_F(QueryLayerTest, ALaterStagesFormKeepsTheNativeRefusal) {
    for (const char* sql : {
             "SELECT c.id FROM cust AS c LEFT JOIN ord AS o ON c.id = o.cust_id",      // S6
             "SELECT c.id FROM cust AS c, ord AS o WHERE c.id = o.cust_id",            // S6
             "SELECT id FROM cust UNION SELECT id FROM ord",                           // S7
             "SELECT d.id FROM (SELECT id FROM cust) AS d",                            // S7
             "SELECT id, (SELECT 1) FROM cust",                                        // S7
         }) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
}

// ---- BU-S5: grouped statements ---------------------------------------------

TEST_F(QueryLayerTest, HavingKeepsAGroupOnlyWhenItsConditionIsTrue) {
    ExpectRows("SELECT cust_id, SUM(amount) FROM ord GROUP BY cust_id HAVING SUM(amount) > 200",
               {"cust_id,sum(amount)", "1,350", "2,375", "4,550"});
    ExpectRows("SELECT status, COUNT(*) FROM ord GROUP BY status "
               "HAVING COUNT(*) > 3 OR MAX(amount) > 400",
               {"status,count(*)", "open,4", "paid,3"});
    // A NULL condition drops the group (BU-R4): below id 17, MAX(note) is
    // NULL for 'open'.
    ExpectRows("SELECT status FROM ord WHERE id < 17 GROUP BY status HAVING MAX(note) != 'zzz'",
               {"status", "cancel", "paid"});
    // HAVING without GROUP BY makes one global group.
    ExpectRows("SELECT COUNT(*) FROM ord HAVING COUNT(*) > 100", {"count(*)"});
}

TEST_F(QueryLayerTest, OrderByAndLimitOverGroups) {
    ExpectRows("SELECT status, COUNT(*) FROM ord GROUP BY status ORDER BY status",
               {"status,count(*)", "cancel,1", "open,4", "paid,3"}, /*ordered=*/true);
    ExpectRows("SELECT cust_id, SUM(amount) FROM ord GROUP BY cust_id "
               "ORDER BY SUM(amount) DESC, cust_id",
               {"cust_id,sum(amount)", "4,550", "2,375", "1,350", "6,120", "99,10"},
               /*ordered=*/true);
    ExpectRows("SELECT cust_id, COUNT(*) AS n FROM ord GROUP BY cust_id ORDER BY n DESC, 1 "
               "LIMIT 2 OFFSET 1",
               {"cust_id,n", "2,2", "4,2"}, /*ordered=*/true);
}

TEST_F(QueryLayerTest, AnAggregateTakesAnAliasAndAnExpression) {
    ExpectRows("SELECT COUNT(*) AS n FROM ord", {"n", "8"});
    ExpectRows("SELECT SUM(amount * 2) FROM ord", {"SUM(amount * 2)", "2810"});
    ExpectRows("SELECT COUNT(*) + 1, MAX(amount) - MIN(amount) AS spread FROM ord",
               {"COUNT(*) + 1,spread", "9,490"});
    // GROUP BY an expression (BU-Q14), a select item reading it as written.
    ExpectRows("SELECT amount / 100, COUNT(*) FROM ord GROUP BY amount / 100",
               {"amount / 100,count(*)", "0,3", "1,2", "2,1", "3,1", "5,1"});
    ExpectRefused("SELECT amount + 1 FROM ord GROUP BY amount / 100", "must appear in GROUP BY");
    ExpectRefused("SELECT SUM(COUNT(*)) FROM ord", "aggregate");
}

TEST_F(QueryLayerTest, AnAggregateInASubquery) {
    ExpectRows("SELECT id FROM cust WHERE id IN "
               "(SELECT cust_id FROM ord GROUP BY cust_id HAVING COUNT(*) > 1)",
               {"id", "1", "2", "4"});
    ExpectRows("SELECT id FROM ord WHERE CAST(amount AS decimal(18,2)) > "
               "(SELECT AVG(CAST(amount AS decimal(18,2))) FROM ord) ORDER BY id",
               {"id", "11", "13", "15"}, /*ordered=*/true);
}

// The native fold checks every addition (aggregate.md AG3): a SUM crossing
// INT64_MAX fails though a later row brings it back.
TEST_F(QueryLayerTest, ASumCrossingInt64MaxFailsAsTheNativeFoldDoes) {
    Ok("CREATE TABLE big (id int64, v int64)");
    Ok("INSERT INTO big VALUES (1, 9223372036854775807)");
    Ok("INSERT INTO big VALUES (2, 1)");
    Ok("INSERT INTO big VALUES (3, -5)");
    ExpectRefused("SELECT SUM(v + 0) FROM big", "SUM overflow");
    ExpectRefused("SELECT SUM(v) FROM big HAVING COUNT(*) > 0", "SUM overflow");
}

}  // namespace
}  // namespace kds::server
