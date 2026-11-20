// **A statement shape's statistics, and what the statement bound** (DI,
// `instructions/v3.2.0/workorder-di-shape-identity.md`).
//
// A fetch id is a hash of a statement's text: it holds no namespace and no
// relation oid. One text therefore names two statements wherever its names
// resolve to two relations - under two namespaces (`USE`, NS5), or over a
// relation dropped and created again under its name (a new oid, DT2).
//
// What this file pins (DI-S1, DI-R2): a `sys.statement_stats` row is one
// binding's. The binding is the distinct relations a statement declared, in
// first-declared order, with the namespace its unqualified names were
// resolved in (`stats/statement_binding.hpp`); a statement that bound
// nothing, and one that bound more than a binding holds, each have a row
// that says so. Until DI-S1 the first two cells ended in one row each.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/statement_binding.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

using stats::StatementBinding;
using stats::StatementShape;
using stats::StatementStats;

// ---- The binding itself -------------------------------------------------------

TEST(StatementBindingTest, RelationsAreDistinctInFirstDeclaredOrder) {
    StatementBinding b;
    EXPECT_TRUE(b.none());
    b.Declare(7, 0);
    b.Declare(5, 0);
    b.Declare(7, 0);
    b.Declare(9, 0);
    b.Declare(5, 0);
    ASSERT_EQ(b.n, 3u);
    EXPECT_EQ(b.oids[0], 7u);
    EXPECT_EQ(b.oids[1], 5u);
    EXPECT_EQ(b.oids[2], 9u);
    EXPECT_EQ(b.namespace_oid, 0u) << "no name was written unqualified";
    EXPECT_FALSE(b.none());

    // The order is part of it: two relations that swapped names are another
    // statement.
    StatementBinding swapped;
    swapped.Declare(5, 0);
    swapped.Declare(7, 0);
    swapped.Declare(9, 0);
    EXPECT_FALSE(b == swapped);
}

TEST(StatementBindingTest, TheNamespaceIsTheOneAnUnqualifiedNameWasResolvedIn) {
    StatementBinding qualified;
    qualified.Declare(7, 0);
    StatementBinding in_three;
    in_three.Declare(7, 3);
    StatementBinding in_four;
    in_four.Declare(7, 4);
    EXPECT_EQ(in_three.namespace_oid, 3u);
    EXPECT_FALSE(qualified == in_three);
    EXPECT_FALSE(in_three == in_four);
    // A qualified name after an unqualified one keeps the namespace.
    in_three.Declare(8, 0);
    EXPECT_EQ(in_three.namespace_oid, 3u);
}

TEST(StatementBindingTest, PastTheCapTheBindingIsNotHeldAndEveryOverflowIsEqual) {
    StatementBinding a;
    for (std::uint64_t oid = 1; oid <= StatementBinding::kMaxRelations; ++oid) a.Declare(oid, 2);
    EXPECT_FALSE(a.over);
    EXPECT_EQ(a.n, StatementBinding::kMaxRelations);
    a.Declare(1, 2);  // a relation already held is no overflow
    EXPECT_FALSE(a.over);
    a.Declare(1000, 2);
    EXPECT_TRUE(a.over);
    EXPECT_EQ(a.n, 0u);
    EXPECT_EQ(a.namespace_oid, 0u);
    EXPECT_FALSE(a.none());
    a.Declare(1001, 5);  // nothing more is taken
    EXPECT_EQ(a.n, 0u);
    EXPECT_EQ(a.namespace_oid, 0u);

    StatementBinding b;
    for (std::uint64_t oid = 100; oid <= 100 + StatementBinding::kMaxRelations; ++oid) {
        b.Declare(oid, 0);
    }
    EXPECT_TRUE(b.over);
    EXPECT_TRUE(a == b) << "an overflow claims nothing, so two are one";
    EXPECT_FALSE(a == StatementBinding{});
}

TEST(StatementBindingTest, ARewindForgetsWhatWasDeclaredSinceTheMark) {
    StatementBinding b;
    b.Declare(7, 0);
    const StatementBinding before = b;
    const StatementBinding::Mark mark = b.mark();
    b.Declare(8, 3);
    b.Declare(9, 3);
    b.Rewind(mark);
    // Equal as a key is: the forgotten oids are cleared, not left behind.
    EXPECT_TRUE(b == before);
    b.Declare(10, 4);
    EXPECT_EQ(b.n, 2u);
    EXPECT_EQ(b.oids[1], 10u);
    EXPECT_EQ(b.namespace_oid, 4u);

    // An overflow since the mark stays one.
    StatementBinding over;
    const StatementBinding::Mark empty = over.mark();
    for (std::uint64_t oid = 1; oid <= StatementBinding::kMaxRelations + 1; ++oid) {
        over.Declare(oid, 0);
    }
    over.Rewind(empty);
    EXPECT_TRUE(over.over);
}

// ---- The statistics' rows -----------------------------------------------------

class ShapeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        d_.emplace(boot_->superblock, boot_->catalog, store_);
        StatementStats::ResetForTest();
    }
    void TearDown() override { StatementStats::ResetForTest(); }

    std::string Run(const std::string& sql) { return d_->Dispatch(sql).response; }

    // Runs `sql` and requires its reply to open with `tag`.
    void Ok(const std::string& sql, std::string_view tag) {
        const std::string reply = Run(sql);
        ASSERT_EQ(reply.substr(0, tag.size()), tag) << sql << " -> " << reply;
    }

    catalog::Oid OidOf(std::string_view ns, std::string_view table) {
        auto oid = boot_->catalog.ResolveRelation(ns, table, catalog::kNamespacePublic);
        EXPECT_TRUE(oid.ok()) << ns << "." << table;
        return oid.ok() ? oid.value() : 0;
    }
    catalog::Oid NamespaceOid(std::string_view ns) {
        auto found = boot_->catalog.FindNamespaceOidByName(ns);
        EXPECT_TRUE(found.ok()) << ns;
        return found.ok() ? found.value() : 0;
    }

    // Every `sys.statement_stats` row of `statement`'s shape and kind.
    static std::vector<StatementShape> RowsOf(std::string_view kind, std::string_view statement) {
        const auto fingerprint = parser::FingerprintOf(statement);
        const std::uint64_t fetch_id = fingerprint.has_value() ? fingerprint->fetch_id : 0;
        std::vector<StatementShape> rows;
        for (StatementShape& s : StatementStats::Snapshot()) {
            if (s.kind == kind && s.fetch_id == fetch_id) rows.push_back(std::move(s));
        }
        return rows;
    }
    static std::vector<catalog::Oid> Relations(const StatementShape& s) {
        return {s.binding.oids.begin(), s.binding.oids.begin() + s.binding.n};
    }

    // `east.acct` (one row) and `west.acct` (three), and `east.memo`,
    // `west.memo` (empty).
    void TwoNamespaces() {
        Ok("CREATE NAMESPACE east", "CREATED");
        Ok("CREATE NAMESPACE west", "CREATED");
        for (const char* ns : {"east", "west"}) {
            Ok(std::string("CREATE TABLE ") + ns + ".acct (id int64, v int64) BTREE", "CREATED");
            Ok(std::string("CREATE TABLE ") + ns + ".memo (id int64, v int64) BTREE", "CREATED");
        }
        Ok("INSERT INTO east.acct (id, v) VALUES (1, 10)", "INSERTED");
        // Row by row: a dispatcher with no transaction manager places one
        // row a statement.
        Ok("INSERT INTO west.acct (id, v) VALUES (1, 20)", "INSERTED");
        Ok("INSERT INTO west.acct (id, v) VALUES (2, 21)", "INSERTED");
        Ok("INSERT INTO west.acct (id, v) VALUES (3, 22)", "INSERTED");
        StatementStats::ResetForTest();
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> d_;
};

TEST_F(ShapeIdentityTest, OneTextUnderTwoNamespacesIsTwoRows) {
    TwoNamespaces();
    const std::string read = "SELECT v FROM acct WHERE id >= 1";
    Ok("USE east", "USE");
    Ok(read, "v");
    Ok("USE west", "USE");
    Ok(read, "v");
    Ok(read, "v");

    const std::vector<StatementShape> rows = RowsOf("SELECT", read);
    ASSERT_EQ(rows.size(), 2u);
    // Ordered by namespace: `east` was created first.
    EXPECT_EQ(rows[0].binding.namespace_oid, NamespaceOid("east"));
    EXPECT_EQ(Relations(rows[0]), std::vector<catalog::Oid>{OidOf("east", "acct")});
    EXPECT_EQ(rows[0].calls, 1u);
    EXPECT_EQ(rows[0].rows, 1u);
    EXPECT_EQ(rows[1].binding.namespace_oid, NamespaceOid("west"));
    EXPECT_EQ(Relations(rows[1]), std::vector<catalog::Oid>{OidOf("west", "acct")});
    EXPECT_EQ(rows[1].calls, 2u);
    EXPECT_EQ(rows[1].rows, 6u);
}

TEST_F(ShapeIdentityTest, OneTextOverARelationDroppedAndCreatedAgainIsTwoRows) {
    const std::string write = "INSERT INTO ledger (id, v) VALUES (1, 1)";
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    const catalog::Oid first = OidOf("public", "ledger");
    Ok(write, "INSERTED");
    Ok("DROP TABLE ledger", "DROPPED");
    // The name again, another relation: its oid is new (DT2).
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    const catalog::Oid second = OidOf("public", "ledger");
    ASSERT_NE(first, second);
    Ok(write, "INSERTED");

    const std::vector<StatementShape> rows = RowsOf("INSERT", write);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(Relations(rows[0]), std::vector<catalog::Oid>{first});
    EXPECT_EQ(Relations(rows[1]), std::vector<catalog::Oid>{second});
    for (const StatementShape& row : rows) {
        EXPECT_EQ(row.binding.namespace_oid, catalog::kNamespacePublic);
        EXPECT_EQ(row.calls, 1u);
        EXPECT_EQ(row.rows, 1u);
    }
}

TEST_F(ShapeIdentityTest, AFullyQualifiedStatementIsOneRowWhateverNamespaceItsSessionIsIn) {
    TwoNamespaces();
    const std::string read = "SELECT v FROM east.acct WHERE id >= 1";
    Ok("USE east", "USE");
    Ok(read, "v");
    Ok("USE west", "USE");
    Ok(read, "v");

    const std::vector<StatementShape> rows = RowsOf("SELECT", read);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].binding.namespace_oid, 0u) << "it wrote no unqualified name";
    EXPECT_EQ(Relations(rows[0]), std::vector<catalog::Oid>{OidOf("east", "acct")});
    EXPECT_EQ(rows[0].calls, 2u);
}

TEST_F(ShapeIdentityTest, TheNamespaceTellsApartWhatTheDistinctRelationsDoNot) {
    // `east.acct JOIN west.acct JOIN acct`: under `USE east` the third name
    // is the first relation, under `USE west` the second. The distinct
    // relations are the same two in the same order; the statements are not
    // the same (`statement_binding.hpp`).
    TwoNamespaces();
    const std::string join =
        "SELECT a.v FROM east.acct AS a JOIN west.acct AS b ON a.id = b.id "
        "JOIN acct AS c ON b.id = c.id";
    Ok("USE east", "USE");
    Ok(join, "a.v");
    Ok("USE west", "USE");
    Ok(join, "a.v");

    const std::vector<StatementShape> rows = RowsOf("SELECT", join);
    ASSERT_EQ(rows.size(), 2u);
    const std::vector<catalog::Oid> both = {OidOf("east", "acct"), OidOf("west", "acct")};
    EXPECT_EQ(Relations(rows[0]), both);
    EXPECT_EQ(Relations(rows[1]), both);
    EXPECT_EQ(rows[0].binding.namespace_oid, NamespaceOid("east"));
    EXPECT_EQ(rows[1].binding.namespace_oid, NamespaceOid("west"));
    EXPECT_EQ(rows[0].calls, 1u);
    EXPECT_EQ(rows[1].calls, 1u);
}

TEST_F(ShapeIdentityTest, AWriteBindsItsTargetAndThenItsSubquerysRelations) {
    TwoNamespaces();
    Ok("USE west", "USE");
    const std::string update = "UPDATE acct SET v = 0 WHERE id IN (SELECT id FROM memo)";
    const std::string del = "DELETE FROM acct WHERE id IN (SELECT id FROM east.memo)";
    Ok(update, "UPDATED");
    Ok(del, "DELETED");

    std::vector<StatementShape> rows = RowsOf("UPDATE", update);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Relations(rows[0]),
              (std::vector<catalog::Oid>{OidOf("west", "acct"), OidOf("west", "memo")}));
    EXPECT_EQ(rows[0].binding.namespace_oid, NamespaceOid("west"));

    rows = RowsOf("DELETE", del);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Relations(rows[0]),
              (std::vector<catalog::Oid>{OidOf("west", "acct"), OidOf("east", "memo")}));
}

TEST_F(ShapeIdentityTest, AQueryLayerStatementBindsEveryRelationItWalksOnce) {
    // A set operation is the query layer's (parser-v2.md I19): one borrow,
    // a walk per relation, and a self-union walks one relation twice.
    TwoNamespaces();
    Ok("USE west", "USE");
    const std::string both = "SELECT v FROM acct UNION ALL SELECT v FROM east.acct";
    const std::string self = "SELECT v FROM acct UNION ALL SELECT v FROM acct";
    Ok(both, "v");
    Ok(self, "v");

    // **The native grammar reads neither, so neither has a fetch id**
    // (`parser.hpp`): every such `SELECT` was one row at fetch id 0, and is
    // now one row a binding - what it resolved to still tells two apart.
    std::vector<StatementShape> rows;
    for (StatementShape& s : StatementStats::Snapshot()) {
        if (s.kind == "SELECT") rows.push_back(std::move(s));
    }
    ASSERT_EQ(rows.size(), 2u);
    for (const StatementShape& row : rows) {
        EXPECT_EQ(row.fetch_id, 0u);
        EXPECT_EQ(row.calls, 1u);
        EXPECT_EQ(row.binding.namespace_oid, NamespaceOid("west"));
    }
    // Ordered by the relations: the self-union's one relation is a prefix
    // of the pair that begins with it, so it sorts first. The pair is in
    // the order the statement names them, not in oid order.
    EXPECT_EQ(Relations(rows[0]), std::vector<catalog::Oid>{OidOf("west", "acct")});
    EXPECT_EQ(Relations(rows[1]),
              (std::vector<catalog::Oid>{OidOf("west", "acct"), OidOf("east", "acct")}));
}

TEST_F(ShapeIdentityTest, AStatementRefusedBeforeItsBindIsTheRowWithNoBinding) {
    TwoNamespaces();
    const std::string read = "SELECT v FROM nowhere WHERE id = 1";
    Ok("USE east", "USE");
    Ok(read, "ERR");
    Ok("USE west", "USE");
    Ok(read, "ERR");

    // Neither bound anything, so nothing tells them apart and nothing is
    // claimed of them.
    const std::vector<StatementShape> rows = RowsOf("SELECT", read);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].binding.none());
    EXPECT_EQ(rows[0].calls, 2u);
    EXPECT_EQ(rows[0].refused, 2u);
}

TEST_F(ShapeIdentityTest, AKindWithNoFingerprintIsOneRowWhateverItNames) {
    Ok("CREATE TABLE one (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE two (id int64, v int64) BTREE", "CREATED");
    Ok("DROP TABLE one", "DROPPED");
    Ok("DROP TABLE two", "DROPPED");
    std::size_t drops = 0;
    for (const StatementShape& s : StatementStats::Snapshot()) {
        if (s.kind != "DROP") continue;
        ++drops;
        EXPECT_EQ(s.fetch_id, 0u);
        EXPECT_TRUE(s.binding.none());
        EXPECT_EQ(s.calls, 2u);
    }
    EXPECT_EQ(drops, 1u);
}

// ---- The views ----------------------------------------------------------------

TEST_F(ShapeIdentityTest, TheViewsNameEachRowsBinding) {
    TwoNamespaces();
    const std::string read = "SELECT v FROM acct WHERE id >= 1";
    Ok("USE east", "USE");
    Ok(read, "v");
    Ok("USE west", "USE");
    Ok(read, "v");
    Ok("SELECT v FROM nowhere WHERE id = 1", "ERR");
    const std::string fetch_id = std::to_string(parser::FingerprintOf(read)->fetch_id);
    const std::string east = std::to_string(OidOf("east", "acct"));
    const std::string west = std::to_string(OidOf("west", "acct"));

    const std::string reply = Run(
        "SELECT fetch_id, kind, binding, namespace, relations, relation_oids, calls "
        "FROM sys.statement_stats");
    const auto stats_view = testing_view::ReadViewText(reply);
    EXPECT_EQ(stats_view.Find("relation_oids", east, "namespace"), "east") << reply;
    EXPECT_EQ(stats_view.Find("relation_oids", east, "relations"), "east.acct") << reply;
    EXPECT_EQ(stats_view.Find("relation_oids", east, "binding"), "bound") << reply;
    EXPECT_EQ(stats_view.Find("relation_oids", east, "fetch_id"), fetch_id) << reply;
    EXPECT_EQ(stats_view.Find("relation_oids", west, "namespace"), "west") << reply;
    EXPECT_EQ(stats_view.Find("relation_oids", west, "relations"), "west.acct") << reply;
    // The refused statement bound nothing, and its row says so.
    const std::string refused = std::to_string(
        parser::FingerprintOf("SELECT v FROM nowhere WHERE id = 1")->fetch_id);
    EXPECT_EQ(stats_view.Find("fetch_id", refused, "binding"), "none") << reply;
    EXPECT_EQ(stats_view.Find("fetch_id", refused, "relations"), "NULL") << reply;
    EXPECT_EQ(stats_view.Find("fetch_id", refused, "relation_oids"), "NULL") << reply;
    EXPECT_EQ(stats_view.Find("fetch_id", refused, "namespace"), "NULL") << reply;

    // The histogram is a row's, so it is a binding's: each binding's buckets
    // sum to its own calls.
    const std::string buckets = Run(
        "SELECT fetch_id, binding, namespace, relation_oids, count FROM sys.statement_histogram");
    const auto histogram = testing_view::ReadViewText(buckets);
    EXPECT_EQ(histogram.Find("relation_oids", east, "count"), "1") << buckets;
    EXPECT_EQ(histogram.Find("relation_oids", west, "count"), "1") << buckets;
    EXPECT_EQ(histogram.Find("relation_oids", west, "namespace"), "west") << buckets;
}

TEST_F(ShapeIdentityTest, ADroppedRelationsRowKeepsItsOidAndSaysTheRelationIsGone) {
    const std::string write = "INSERT INTO ledger (id, v) VALUES (1, 1)";
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    const std::string first = std::to_string(OidOf("public", "ledger"));
    Ok(write, "INSERTED");
    Ok("DROP TABLE ledger", "DROPPED");
    Ok("CREATE TABLE ledger (id int64, v int64) BTREE", "CREATED");
    const std::string second = std::to_string(OidOf("public", "ledger"));
    Ok(write, "INSERTED");

    const std::string reply =
        Run("SELECT kind, namespace, relations, relation_oids, calls FROM sys.statement_stats");
    const auto view = testing_view::ReadViewText(reply);
    EXPECT_EQ(view.Find("relation_oids", first, "relations"), "oid " + first) << reply;
    EXPECT_EQ(view.Find("relation_oids", second, "relations"), "public.ledger") << reply;
    EXPECT_EQ(view.Find("relation_oids", first, "namespace"), "public") << reply;
    EXPECT_EQ(view.Find("relation_oids", first, "calls"), "1") << reply;
}

// ---- A full part (DI-Q7) ------------------------------------------------------

TEST_F(ShapeIdentityTest, AFullPartGivesAGoneRelationsRowsToANewShape) {
    Ok("CREATE TABLE gone (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE kept (id int64, v int64) BTREE", "CREATED");
    const catalog::Oid gone = OidOf("public", "gone");
    const catalog::Oid kept = OidOf("public", "kept");
    Ok("DROP TABLE gone", "DROPPED");
    StatementStats::ResetForTest();

    // The part, filled: three shapes over the dropped relation, the rest
    // over the live one.
    StatementBinding over_gone;
    over_gone.Declare(gone, catalog::kNamespacePublic);
    StatementBinding over_kept;
    over_kept.Declare(kept, catalog::kNamespacePublic);
    for (std::size_t i = 0; i < StatementStats::kShapesPerCore; ++i) {
        StatementStats::Record(0, 1000 + i, "SELECT 1", false, 1, 1, {}, {}, {},
                               i < 3 ? over_gone : over_kept);
    }
    ASSERT_EQ(StatementStats::Snapshot().size(), StatementStats::kShapesPerCore);
    ASSERT_EQ(StatementStats::Dropped(), 0u);

    // A new shape arrives: the dropped relation's three rows go, and it is
    // recorded.
    const std::string read = "SELECT v FROM kept WHERE id = 1";
    Ok(read, "v");
    EXPECT_EQ(RowsOf("SELECT", read).size(), 1u);
    EXPECT_EQ(StatementStats::Dropped(), 0u);
    EXPECT_EQ(StatementStats::Reclaimed(), 3u);
    for (const StatementShape& s : StatementStats::Snapshot()) {
        EXPECT_FALSE(s.binding.n == 1 && s.binding.oids[0] == gone) << s.fetch_id;
    }
    // Two more new shapes fit the room left, and the fourth - with nothing
    // gone to reclaim - is dropped and counted, as before DI. (A shape is
    // its statement without its select list, so each differs in its WHERE.)
    Ok("SELECT v FROM kept WHERE id >= 1", "v");
    Ok("SELECT v FROM kept WHERE v = 1", "v");
    EXPECT_EQ(StatementStats::Dropped(), 0u);
    Ok("SELECT v FROM kept WHERE v >= 1", "v");
    EXPECT_EQ(StatementStats::Dropped(), 1u);
    EXPECT_EQ(StatementStats::Reclaimed(), 3u);

    // `sys.meta` carries both counts, as they stood when it was read: its
    // own statement is recorded at its end.
    const auto meta = testing_view::ReadViewText(
        Run("SELECT field, value FROM sys.meta WHERE section = 'statement_stats'"));
    EXPECT_EQ(meta.Find("field", "statement_stats_reclaimed", "value"), "3");
    EXPECT_EQ(meta.Find("field", "statement_stats_dropped", "value"), "1");
}

}  // namespace
}  // namespace kds::server
