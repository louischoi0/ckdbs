// **A statement shape's statistics, and what the statement bound** (DI,
// `instructions/v3.2.0/workorder-di-shape-identity.md`).
//
// A fetch id is a hash of a statement's text: it holds no namespace and no
// relation oid. One text therefore names two statements wherever its names
// resolve to two relations - under two namespaces (`USE`, NS5), or over a
// relation dropped and created again under its name (a new oid, DT2).
//
// DI-S0's cells show what `sys.statement_stats` makes of the two today: one
// row, the sum of both, with no mark that it is one. They pin the premise
// and are the cells DI-S1 turns over.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class ShapeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        d_.emplace(boot_->superblock, boot_->catalog, store_);
        stats::StatementStats::ResetForTest();
    }
    void TearDown() override { stats::StatementStats::ResetForTest(); }

    // Runs `sql` and requires its reply to open with `tag`.
    void Ok(const std::string& sql, std::string_view tag) {
        const std::string reply = d_->Dispatch(sql).response;
        ASSERT_EQ(reply.substr(0, tag.size()), tag) << sql << " -> " << reply;
    }

    // Every `sys.statement_stats` row of `statement`'s shape and kind.
    static std::vector<stats::StatementShape> RowsOf(std::string_view kind,
                                                     std::string_view statement) {
        const auto fingerprint = parser::FingerprintOf(statement);
        const std::uint64_t fetch_id = fingerprint.has_value() ? fingerprint->fetch_id : 0;
        std::vector<stats::StatementShape> rows;
        for (stats::StatementShape& s : stats::StatementStats::Snapshot()) {
            if (s.kind == kind && s.fetch_id == fetch_id) rows.push_back(std::move(s));
        }
        return rows;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> d_;
};

TEST_F(ShapeIdentityTest, OneTextUnderTwoNamespacesIsOneStatisticsRowToday) {
    Ok("CREATE NAMESPACE east", "CREATED");
    Ok("CREATE NAMESPACE west", "CREATED");
    Ok("CREATE TABLE east.acct (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE west.acct (id int64, v int64) BTREE", "CREATED");
    Ok("INSERT INTO east.acct (id, v) VALUES (1, 10)", "INSERTED");
    // Row by row: a dispatcher with no transaction manager places one row a
    // statement.
    Ok("INSERT INTO west.acct (id, v) VALUES (1, 20)", "INSERTED");
    Ok("INSERT INTO west.acct (id, v) VALUES (2, 21)", "INSERTED");
    Ok("INSERT INTO west.acct (id, v) VALUES (3, 22)", "INSERTED");

    const std::string read = "SELECT v FROM acct WHERE id >= 1";
    Ok("USE east", "USE");
    Ok(read, "v");
    Ok("USE west", "USE");
    Ok(read, "v");

    // Two statements - one row of `east.acct`, three of `west.acct` - and
    // one statistics row holding both.
    const std::vector<stats::StatementShape> rows = RowsOf("SELECT", read);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].calls, 2u);
    EXPECT_EQ(rows[0].rows, 4u);
}

TEST_F(ShapeIdentityTest, OneTextOverARelationDroppedAndCreatedAgainIsOneStatisticsRowToday) {
    const std::string write = "INSERT INTO ledger (id, v) VALUES (1, 1)";
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    Ok(write, "INSERTED");
    Ok("DROP TABLE ledger", "DROPPED");
    // The name again, another relation: its oid is new (DT2).
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    Ok(write, "INSERTED");

    const std::vector<stats::StatementShape> rows = RowsOf("INSERT", write);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].calls, 2u);
    EXPECT_EQ(rows[0].rows, 2u);
}

}  // namespace
}  // namespace kds::server
