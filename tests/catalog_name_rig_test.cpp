// **One name, one row, across a transaction that has not decided** (AT-S17,
// the AT-close order's Q2), on the two-core rig.
//
// The held page (`catalog_name_race_test.cpp`) makes a name's check and
// its write one act, and it holds the name only until the latch drops. A
// `CREATE` or a `DROP` inside an open transaction decides later than that,
// so what keeps a second core from taking the name in between is what the
// check *sees*:
//
//   - **an uncommitted create** is seen because the check is unfiltered
//     (`ddl-transactional.md`'s duplicate check) - the row is on the page
//     whoever wrote it;
//   - **an uncommitted drop** is not seen by that check at all - the retype
//     already hid the name - and its rollback brings the name back. Before
//     AT-S17 the only defence was the dispatcher's pending-drop test, which
//     ran only when *the asking core* had DDL open, so a create, a namespace
//     create or a rename on the other core took the name, and the drop's
//     rollback left two rows claiming it.
//
// The statements are driven synchronously from the test thread, before the
// reactors start: the question is what one core's catalog read sees of
// another core's transaction, not an interleaving.

#include "two_core_rig.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

int RelationsNamed(TwoCoreRig& rig, std::string_view name) {
    auto tables = rig.core(0).catalog().ListTables();
    EXPECT_TRUE(tables.ok()) << tables.status().message();
    int rows = 0;
    if (tables.ok()) {
        for (const catalog::SysObjectRow& row : tables.value()) {
            rows += catalog::NameView(row.name) == name ? 1 : 0;
        }
    }
    return rows;
}

int NamespacesNamed(TwoCoreRig& rig, std::string_view name) {
    auto spaces = rig.core(0).catalog().ListNamespaces();
    EXPECT_TRUE(spaces.ok()) << spaces.status().message();
    int rows = 0;
    if (spaces.ok()) {
        for (const catalog::SysObjectRow& row : spaces.value()) {
            rows += catalog::NameView(row.name) == name ? 1 : 0;
        }
    }
    return rows;
}

TEST(CatalogNameRigTest, ACreateOnOneCoreSeesAnotherCoresUncommittedCreateOfTheName) {
    // The work order's transactional cell: core 0 creates `t` inside an open
    // transaction, and core 1's create of `t` is refused - `EXISTS`, the
    // duplicate check's own answer - rather than admitted beside it.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    const std::string made = d0.Dispatch("CREATE TABLE t (id int64, v int64)", &ddl).response;
    ASSERT_TRUE(StartsWith(made, "CREATED")) << made;

    const std::string second = d1.Dispatch("CREATE TABLE t (id int64, w int64)").response;
    EXPECT_TRUE(StartsWith(second, "EXISTS oid=")) << second;

    ASSERT_TRUE(StartsWith(d0.Dispatch("COMMIT", &ddl).response, "COMMIT"));
    EXPECT_EQ(RelationsNamed(*rig, "t"), 1);
}

TEST(CatalogNameRigTest, ACreateOnOneCoreIsRefusedWhileAnotherCoresDropOfTheNameIsOpen) {
    // Red at `4d1e970`: core 1 answered `CREATED`, and the rollback below
    // left two relations named `t`.
    //
    // **Mutation**: drop the pending-drop arm of `Catalog::CheckNameFree` -
    // this cell, the rename's and the namespace's each fail, 3 runs in 3.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    const std::string dropped = d0.Dispatch("DROP TABLE t", &ddl).response;
    ASSERT_TRUE(StartsWith(dropped, "DROPPED TABLE")) << dropped;

    const std::string second = d1.Dispatch("CREATE TABLE t (id int64, w int64)").response;
    EXPECT_TRUE(StartsWith(second, "ERR")) << second;
    EXPECT_NE(second.find("being dropped"), std::string::npos) << second;

    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    EXPECT_EQ(RelationsNamed(*rig, "t"), 1);
}

TEST(CatalogNameRigTest, ARenameOnOneCoreIsRefusedWhileAnotherCoresDropOfTheNameIsOpen) {
    // `RENAME TO` had no pending-drop test on any core, and it is not
    // transactional - so the rename lands for good and the drop's rollback
    // restores the name beside it. Red at `4d1e970`.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE u (id int64, v int64)").response, "CREATED"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP TABLE t", &ddl).response, "DROPPED TABLE"));

    const std::string renamed = d1.Dispatch("ALTER TABLE u RENAME TO t").response;
    EXPECT_TRUE(StartsWith(renamed, "ERR")) << renamed;
    EXPECT_NE(renamed.find("being dropped"), std::string::npos) << renamed;

    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    EXPECT_EQ(RelationsNamed(*rig, "t"), 1);
    EXPECT_EQ(RelationsNamed(*rig, "u"), 1);
}

TEST(CatalogNameRigTest, ANamespaceCreateOnOneCoreIsRefusedWhileAnotherCoresDropOfItIsOpen) {
    // `CreateNamespace` carried the comment that owed this check "the moment
    // `CREATE NAMESPACE` becomes a transactional statement two sessions can
    // run at once"; AF-T3 made it one. Red at `4d1e970`.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE NAMESPACE ledger").response, "CREATED"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    const std::string dropped = d0.Dispatch("DROP NAMESPACE ledger", &ddl).response;
    ASSERT_TRUE(StartsWith(dropped, "DROPPED NAMESPACE")) << dropped;

    const std::string second = d1.Dispatch("CREATE NAMESPACE ledger").response;
    EXPECT_TRUE(StartsWith(second, "ERR")) << second;
    EXPECT_NE(second.find("being dropped"), std::string::npos) << second;

    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    EXPECT_EQ(NamespacesNamed(*rig, "ledger"), 1);
}

TEST(CatalogNameRigTest, AnIndexCreateOnOneCoreIsRefusedWhileAnotherCoresDropOfItIsOpen) {
    // The index flavour, found by AT-S17's survey: a `DROP INDEX` inside a
    // transaction delete-marks its row, and the unfiltered name check read
    // the mark by *its own core's* in-flight test (DT9), so core 1 saw core
    // 0's open drop as done and took the name on another relation. The
    // rollback cleared the mark: two indexes of one name.
    //
    // **Mutation**: `CheckIndexNameFree` settling a mark through `txn_`'s
    // `IsInFlight` rather than the check view - core 1 answers `CREATED`,
    // 3 runs in 3.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE p (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE q (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE INDEX ix ON p (v)").response, "CREATED INDEX"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    const std::string dropped = d0.Dispatch("DROP INDEX ix", &ddl).response;
    ASSERT_TRUE(StartsWith(dropped, "DROPPED INDEX")) << dropped;

    const std::string second = d1.Dispatch("CREATE INDEX ix ON q (v)").response;
    EXPECT_TRUE(StartsWith(second, "ERR")) << second;
    EXPECT_NE(second.find("being dropped"), std::string::npos) << second;

    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    auto indexes = rig->core(0).catalog().ListIndexes();
    ASSERT_TRUE(indexes.ok()) << indexes.status().message();
    int named = 0;
    for (const catalog::SysIndexRow& row : indexes.value()) {
        named += catalog::NameView(row.name) == "ix" ? 1 : 0;
    }
    EXPECT_EQ(named, 1);
}

TEST(CatalogNameRigTest, ItsOwnOpenDropIsRefusedAsNotRetryable) {
    // The two own-transaction cases, which a retry can never clear because
    // the only transaction they wait on is the asker's (AT-S17's review,
    // findings 1 and 2):
    //
    //   - recreating an index inside the transaction that drops it keeps
    //     the refusal it had before AT-S17 - the name is the dropped
    //     index's until the drop commits - and not `TXN_CONFLICT`;
    //   - renaming onto a name the transaction is dropping is refused
    //     `UNSUPPORTED`: the rename is not undone by the drop's rollback.
    //
    // `DROP TABLE t; CREATE TABLE t` in one transaction stays admitted
    // (`ATransactionMayDropAndRecreateOneNameItself`).
    //
    // **Mutations**: either check ignoring the asker's own id - each half
    // answers `TXN_CONFLICT retryable=1`, 3 runs in 3.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE u (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE INDEX ix ON t (v)").response, "CREATED INDEX"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP INDEX ix", &ddl).response, "DROPPED INDEX"));
    const std::string again = d0.Dispatch("CREATE INDEX ix ON t (v)", &ddl).response;
    EXPECT_TRUE(StartsWith(again, "ERR")) << again;
    EXPECT_NE(again.find("already exists"), std::string::npos) << again;
    EXPECT_EQ(again.find("retryable=1"), std::string::npos) << again;
    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));

    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP TABLE t", &ddl).response, "DROPPED TABLE"));
    const std::string renamed = d0.Dispatch("ALTER TABLE u RENAME TO t", &ddl).response;
    EXPECT_TRUE(StartsWith(renamed, "ERR UNSUPPORTED retryable=0")) << renamed;
    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    EXPECT_EQ(RelationsNamed(*rig, "t"), 1);
    EXPECT_EQ(RelationsNamed(*rig, "u"), 1);
}

}  // namespace
}  // namespace kds::server
