#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// docs/txn.md sections 10-5, 10-6, 10-7 and 10-8, end to end: two sessions
// on **one dispatcher**, which is what the shared-dispatcher design makes
// the interesting case. Deterministic and socket-free (rules.md section 4).
//
// The unlogged path throughout - no WalManager - because what is under test
// is what a client sees, not what reaches the platter. The record-level
// half is insert_wal_test.cpp's and undo_log_test.cpp's.

namespace kds::server {
namespace {

class TxnSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    // Rows of a SELECT as "a|b" strings, dropping the header line. The
    // dispatcher answers one wire line with "\n" as a two-character escape.
    std::vector<std::string> Rows(Session& s, const std::string& sql) {
        const std::string reply = Run(s, sql);
        std::vector<std::string> out;
        std::size_t at = 0;
        bool first = true;
        while (at <= reply.size()) {
            const std::size_t next = reply.find("\\n", at);
            const std::string piece =
                reply.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!first) out.push_back(piece);
            first = false;
            if (next == std::string::npos) break;
            at = next + 2;
        }
        return out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The state machine (section 10-8) ------------------------------------

TEST_F(TxnSessionTest, BeginCommitAndRollbackMoveTheSessionThroughItsStates) {
    Session s;
    EXPECT_EQ(s.state(), Session::State::kIdle);

    EXPECT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(s.state(), Session::State::kInTxn);
    EXPECT_NE(s.transaction(), nullptr);

    EXPECT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(s.state(), Session::State::kIdle);
    EXPECT_EQ(s.transaction(), nullptr);

    EXPECT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(s.state(), Session::State::kIdle);
}

TEST_F(TxnSessionTest, CommitOrRollbackOutsideATransactionIsAnError) {
    Session s;
    EXPECT_EQ(Run(s, "COMMIT"), "ERR no transaction is open");
    EXPECT_EQ(Run(s, "ROLLBACK"), "ERR no transaction is open");
}

// No savepoints (section 9), so a second BEGIN has no reading that is not a
// guess about which transaction a later COMMIT ends.
TEST_F(TxnSessionTest, ASecondBeginIsRefusedRatherThanNested) {
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s, "BEGIN"), "ERR a transaction is already open; COMMIT or ROLLBACK first");
}

TEST_F(TxnSessionTest, AFailedStatementPoisonsTheTransactionAndOnlyTheWayOutIsAdmitted) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES (1)").substr(0, 8), "INSERTED");

    // A statement that fails inside the transaction.
    EXPECT_EQ(Run(s, "UPDATE t SET nosuch = 2").substr(0, 3), "ERR");
    EXPECT_EQ(s.state(), Session::State::kFailedTxn);

    // Everything but the ways out is refused - a whitelist, so a statement
    // added later is refused by default rather than admitted by omission.
    for (const char* sql : {"SELECT id, v FROM t", "INSERT INTO t VALUES (2)",
                            "UPDATE t SET v = 9", "COMMIT", "SHOW TABLES"}) {
        EXPECT_EQ(Run(s, sql),
                  "ERR current transaction is aborted; commands are ignored until ROLLBACK")
            << sql;
    }
    EXPECT_EQ(Run(s, "PING"), "PONG");
    EXPECT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(s.state(), Session::State::kIdle);

    // And the insert inside the poisoned transaction was undone.
    Session after;
    EXPECT_TRUE(Rows(after, "SELECT id, v FROM t").empty());
}

TEST_F(TxnSessionTest, TwoSessionsOnOneDispatcherDoNotInterfere) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(b.state(), Session::State::kIdle) << "one session's BEGIN is not the other's";
    EXPECT_EQ(Run(b, "COMMIT"), "ERR no transaction is open");

    ASSERT_EQ(Run(b, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_NE(a.transaction()->id(), b.transaction()->id());

    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(b.state(), Session::State::kInTxn);
    ASSERT_EQ(Run(b, "COMMIT").substr(0, 6), "COMMIT");
}

// ---- RC vs RR (section 10-5) ---------------------------------------------

TEST_F(TxnSessionTest, RepeatableReadHoldsOneViewAcrossStatements) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session reader;
    Session writer;
    ASSERT_EQ(Run(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ").substr(0, 5), "BEGIN");
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));

    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "UPDATE t SET v = 99"), "UPDATED 1");
    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");

    // The whole point of REPEATABLE READ: the second SELECT sees what the
    // first did, even though the update committed in between.
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));
    ASSERT_EQ(Run(reader, "COMMIT").substr(0, 6), "COMMIT");

    // ...and only after COMMIT does it see the new value.
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,99"}));
}

TEST_F(TxnSessionTest, ReadCommittedResnapshotsAtEveryStatement) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session reader;
    Session writer;
    ASSERT_EQ(Run(reader, "BEGIN ISOLATION LEVEL READ COMMITTED").substr(0, 5), "BEGIN");
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));

    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "UPDATE t SET v = 99"), "UPDATED 1");

    // Still uncommitted: invisible even under READ COMMITTED.
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));

    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");

    // The same script as the REPEATABLE READ case above, and here the
    // second SELECT *does* see the new value. That difference is the only
    // observable difference between the two levels.
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,99"}));
    ASSERT_EQ(Run(reader, "COMMIT").substr(0, 6), "COMMIT");
}

TEST_F(TxnSessionTest, AnUncommittedWriteIsInvisibleToOthersAndVisibleToItsWriter) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session writer;
    Session other;
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "UPDATE t SET v = 42"), "UPDATED 1");

    EXPECT_EQ(Rows(writer, "SELECT id, v FROM t"), (std::vector<std::string>{"1,42"}))
        << "a transaction always sees its own writes";
    EXPECT_EQ(Rows(other, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}))
        << "and nobody else does until it commits";

    ASSERT_EQ(Run(writer, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Rows(other, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));
    EXPECT_EQ(Rows(writer, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));
}

TEST_F(TxnSessionTest, AnUncommittedInsertIsInvisibleToOthers) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");

    Session writer;
    Session other;
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "INSERT INTO t VALUES (7)").substr(0, 8), "INSERTED");

    // undo_ptr == 0 plus an invisible writer means "no visible version",
    // which is the whole reason an INSERT writes no undo record (3.6).
    EXPECT_TRUE(Rows(other, "SELECT id, v FROM t").empty());
    EXPECT_EQ(Rows(writer, "SELECT id, v FROM t"), (std::vector<std::string>{"1,7"}));

    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Rows(other, "SELECT id, v FROM t"), (std::vector<std::string>{"1,7"}));
}

// ---- Conflicts (section 10-6) --------------------------------------------

TEST_F(TxnSessionTest, TwoWritersOnOneRowGiveTheLoserARetryableConflict) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session s1;
    Session s2;
    ASSERT_EQ(Run(s1, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s2, "BEGIN").substr(0, 5), "BEGIN");

    ASSERT_EQ(Run(s1, "UPDATE t SET v = 1"), "UPDATED 1");

    // The spelling is the wire contract, not a diagnostic: client retry
    // loops read the `retryable` bit (protocol.md section 11).
    const std::string conflict = Run(s2, "UPDATE t SET v = 2");
    EXPECT_EQ(conflict.substr(0, 29), "ERR TXN_CONFLICT retryable=1 ") << conflict;
    EXPECT_NE(conflict.find("row id=1"), std::string::npos) << conflict;
    EXPECT_NE(conflict.find("was written by transaction"), std::string::npos) << conflict;
    EXPECT_EQ(s2.state(), Session::State::kFailedTxn);

    // After the winner rolls back, the loser's retry succeeds.
    ASSERT_EQ(Run(s1, "ROLLBACK").substr(0, 8), "ROLLBACK");
    ASSERT_EQ(Run(s2, "ROLLBACK").substr(0, 8), "ROLLBACK");
    ASSERT_EQ(Run(s2, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s2, "UPDATE t SET v = 2"), "UPDATED 1");
    ASSERT_EQ(Run(s2, "COMMIT").substr(0, 6), "COMMIT");

    Session reader;
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,2"}));
}

// My own earlier write is not a conflict: the second undo record links to
// the first, so a rollback unwinds both and lands on the **original**.
TEST_F(TxnSessionTest, ADoubleUpdateInOneTransactionRollsBackToTheOriginal) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s, "UPDATE t SET v = 20"), "UPDATED 1");
    EXPECT_EQ(Run(s, "UPDATE t SET v = 30"), "UPDATED 1") << "my own write is not a conflict";
    EXPECT_EQ(Rows(s, "SELECT id, v FROM t"), (std::vector<std::string>{"1,30"}));

    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    Session reader;
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}))
        << "unwinding one link would have left 20";
}

// ---- Rollback (section 10-7) ---------------------------------------------

TEST_F(TxnSessionTest, RollbackUndoesEveryStatementOfAMultiRowTransaction) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");
    }

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s, "UPDATE t SET v = 99"), "UPDATED 3");
    EXPECT_EQ(Run(s, "INSERT INTO t VALUES (55)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Rows(s, "SELECT id, v FROM t").size(), 4u);

    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    Session reader;
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"),
              (std::vector<std::string>{"1,10", "2,10", "3,10"}))
        << "the inserted row must be gone and the three updates put back";
}

// Autocommit is statement-atomic, because the statement *is* the
// transaction and EndWrite aborts it (section 6).
TEST_F(TxnSessionTest, AnAutocommitStatementCommitsOnItsOwn) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");
    EXPECT_EQ(s.state(), Session::State::kIdle) << "no transaction is left open";

    Session other;
    EXPECT_EQ(Rows(other, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}))
        << "an autocommit write is visible to everyone immediately";
}

// ---- Isolation-level plumbing (section 1's precedence chain) -------------

TEST_F(TxnSessionTest, SetIsolationLevelAppliesToTheNextTransaction) {
    Session s;
    EXPECT_EQ(s.isolation(), txn::IsolationLevel::kReadCommitted);
    EXPECT_EQ(Run(s, "SET ISOLATION LEVEL REPEATABLE READ"), "SET isolation=repeatable read");
    EXPECT_EQ(s.isolation(), txn::IsolationLevel::kRepeatableRead);

    ASSERT_NE(Run(s, "BEGIN").find("isolation=repeatable read"), std::string::npos);
    // ...and it cannot be changed underneath a running transaction.
    EXPECT_EQ(Run(s, "SET ISOLATION LEVEL READ COMMITTED"),
              "ERR cannot change the isolation level inside a transaction");
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
}

TEST_F(TxnSessionTest, BeginOverridesTheSessionLevelForOneTransactionOnly) {
    Session s;
    ASSERT_NE(Run(s, "BEGIN ISOLATION LEVEL REPEATABLE READ").find("repeatable read"),
              std::string::npos);
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(s.isolation(), txn::IsolationLevel::kReadCommitted)
        << "the override was for that transaction, not for the session";
}

TEST_F(TxnSessionTest, AnUnknownIsolationLevelIsRefusedAndSerializableSaysWhy) {
    Session s;
    EXPECT_NE(Run(s, "BEGIN ISOLATION LEVEL SNAPSHOT").find("unknown isolation level"),
              std::string::npos);
    EXPECT_EQ(s.state(), Session::State::kIdle) << "a refused BEGIN opens nothing";
    EXPECT_NE(Run(s, "BEGIN ISOLATION LEVEL SERIALIZABLE").find("predicate locking"),
              std::string::npos);
}

// A dispatcher built without a manager - every pre-existing test - refuses
// transaction control rather than pretending to support it.
TEST(TxnSessionNoManagerTest, TransactionControlIsRefusedWithoutAManager) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 4000);
    ASSERT_TRUE(boot.ok());
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store);

    Session s;
    EXPECT_EQ(d.Dispatch("BEGIN", &s).response,
              "ERR this server was built without a transaction manager");
    EXPECT_EQ(s.state(), Session::State::kIdle);

    // ...and ordinary statements still work exactly as they always did.
    EXPECT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)", &s).response.substr(0, 7),
              "CREATED");
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (5)", &s).response.substr(0, 8), "INSERTED");
    EXPECT_EQ(d.Dispatch("SELECT id, v FROM t", &s).response.find("1,5") != std::string::npos,
              true);
}

// ---- DELETE (docs/txn.md sections 4.3, 6) --------------------------------

TEST_F(TxnSessionTest, DeleteMarksRatherThanRemovesAndAnOlderViewStillSeesTheRow) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (20)").substr(0, 8), "INSERTED");

    Session reader;
    ASSERT_EQ(Run(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ").substr(0, 5), "BEGIN");
    ASSERT_EQ(Rows(reader, "SELECT id, v FROM t").size(), 2u);

    Session deleter;
    EXPECT_EQ(Run(deleter, "DELETE FROM t WHERE id = 1"), "DELETED 1");

    // The row is gone for a view taken after the delete...
    Session after;
    EXPECT_EQ(Rows(after, "SELECT id, v FROM t"), (std::vector<std::string>{"2,20"}));

    // ...and still there for the one taken before it. A delete-mark carries
    // no bytes, so stepping back over it keeps the tuple's own payload.
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"),
              (std::vector<std::string>{"1,10", "2,20"}));
    ASSERT_EQ(Run(reader, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"2,20"}));
}

TEST_F(TxnSessionTest, RollbackClearsADeleteMark) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s, "DELETE FROM t"), "DELETED 1");
    EXPECT_TRUE(Rows(s, "SELECT id, v FROM t").empty());

    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    Session reader;
    EXPECT_EQ(Rows(reader, "SELECT id, v FROM t"), (std::vector<std::string>{"1,10"}));
}

TEST_F(TxnSessionTest, DeleteWithNoWhereRemovesEveryVisibleRow) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(Run(s, "INSERT INTO t VALUES (1)").substr(0, 8), "INSERTED");
    }
    EXPECT_EQ(Run(s, "DELETE FROM t"), "DELETED 3");
    EXPECT_TRUE(Rows(s, "SELECT id, v FROM t").empty());

    // Deleting again marks nothing: a row already gone for this reader has
    // no version to delete.
    EXPECT_EQ(Run(s, "DELETE FROM t"), "DELETED 0");
}

TEST_F(TxnSessionTest, TwoDeletersOnOneRowConflict) {
    Session setup;
    ASSERT_EQ(Run(setup, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(setup, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    Session s1;
    Session s2;
    ASSERT_EQ(Run(s1, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s2, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(s1, "DELETE FROM t WHERE id = 1"), "DELETED 1");

    const std::string conflict = Run(s2, "DELETE FROM t WHERE id = 1");
    EXPECT_EQ(conflict.substr(0, 29), "ERR TXN_CONFLICT retryable=1 ") << conflict;
    EXPECT_EQ(s2.state(), Session::State::kFailedTxn);
}

// An UPDATE must not resurrect a row a committed DELETE removed.
TEST_F(TxnSessionTest, AnUpdateSkipsARowAlreadyDeleted) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "DELETE FROM t WHERE id = 1"), "DELETED 1");

    EXPECT_EQ(Run(s, "UPDATE t SET v = 99"), "UPDATED 0");
    EXPECT_TRUE(Rows(s, "SELECT id, v FROM t").empty());
}

// ---- Transactional DDL at the SQL surface (DT3b) ------------------------

TEST_F(TxnSessionTest, ARolledBackCreateTableLeavesNoRelation) {
    // What `docs/txn.md` §7 said was a known limitation until 2026-08-15:
    // "CREATE TABLE inside an explicit transaction is not rolled back by
    // ROLLBACK". This is that sentence becoming false.
    Session s;
    EXPECT_EQ(Run(s, "BEGIN").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(s, "CREATE TABLE gone (id int64, v int64)").rfind("CREATED", 0), 0u);
    EXPECT_EQ(Run(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);

    // Gone for good: the rows were retired, not hidden - so even the same
    // session, outside any transaction, cannot find it.
    EXPECT_EQ(Run(s, "DESCRIBE gone").rfind("ERR", 0), 0u)
        << "the rolled-back relation is still in the catalog";

    // And the name is free again, which is the property a migration script
    // that failed halfway actually needs.
    EXPECT_EQ(Run(s, "CREATE TABLE gone (id int64, v int64)").rfind("CREATED", 0), 0u);
}

TEST_F(TxnSessionTest, ACommittedCreateTableSurvivesAndItsRowsAreUsable) {
    // The other half, and the one that would break quietly if the trail
    // registration were wrong: a *committed* DDL must not be retired.
    Session s;
    EXPECT_EQ(Run(s, "BEGIN").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(s, "CREATE TABLE kept (id int64, v int64)").rfind("CREATED", 0), 0u);
    EXPECT_EQ(Run(s, "INSERT INTO kept VALUES (7)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(Run(s, "COMMIT").rfind("COMMIT", 0), 0u);

    EXPECT_EQ(Rows(s, "SELECT id, v FROM kept"), (std::vector<std::string>{"1,7"}));
}

TEST_F(TxnSessionTest, AutocommitDdlIsUnchangedAndIsNotRolledBackByALaterAbort) {
    // DDL outside an explicit transaction commits as it always did - it
    // joins no trail, so a later unrelated rollback cannot take it back.
    Session s;
    EXPECT_EQ(Run(s, "CREATE TABLE standing (id int64, v int64)").rfind("CREATED", 0), 0u);

    EXPECT_EQ(Run(s, "BEGIN").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(s, "INSERT INTO standing VALUES (1)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(Run(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);

    EXPECT_EQ(Run(s, "DESCRIBE standing").rfind("ERR", 0), std::string::npos)
        << "an autocommit CREATE TABLE was undone by an unrelated rollback";
    EXPECT_TRUE(Rows(s, "SELECT id, v FROM standing").empty());
}

TEST_F(TxnSessionTest, AnUncommittedCreateTableIsInvisibleToEveryOtherSession) {
    // Isolation at the SQL surface (DT3c): three routes into a relation -
    // DESCRIBE, SELECT and INSERT - and none of them may find one whose
    // creating transaction has not committed.
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "CREATE TABLE secret (id int64, v int64)").substr(0, 7), "CREATED");

    // The creator sees its own work by all three routes.
    EXPECT_EQ(Run(a, "DESCRIBE secret").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run(a, "INSERT INTO secret VALUES (1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Rows(a, "SELECT id, v FROM secret"), (std::vector<std::string>{"1,1"}));

    // Another session, in a transaction of its own, sees none of it.
    ASSERT_EQ(Run(b, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(b, "DESCRIBE secret").rfind("ERR", 0), 0u) << "DESCRIBE leaked it";
    EXPECT_EQ(Run(b, "SELECT id, v FROM secret").rfind("ERR", 0), 0u) << "SELECT leaked it";
    EXPECT_EQ(Run(b, "INSERT INTO secret VALUES (2)").rfind("ERR", 0), 0u)
        << "INSERT leaked it";
    ASSERT_EQ(Run(b, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // ...and an autocommit session sees none of it either, which is the
    // case that matters most because it is the common one.
    Session plain;
    EXPECT_EQ(Run(plain, "DESCRIBE secret").rfind("ERR", 0), 0u)
        << "an autocommit reader saw an uncommitted relation";
    EXPECT_EQ(Run(plain, "SELECT id, v FROM secret").rfind("ERR", 0), 0u);

    // Once it commits, everybody sees it - by every route.
    ASSERT_EQ(Run(a, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Run(plain, "DESCRIBE secret").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Rows(plain, "SELECT id, v FROM secret"), (std::vector<std::string>{"1,1"}));
    EXPECT_EQ(Run(plain, "INSERT INTO secret VALUES (3)").substr(0, 8), "INSERTED");
}

TEST_F(TxnSessionTest, WithNoDdlInFlightResolutionStillServesFromTheCache) {
    // The decision DT3c takes (spec §6): a view is minted **only** while
    // some transaction holds uncommitted DDL, because a filtered lookup
    // bypasses the shared cache by design. With none in flight - the
    // normal state - every catalog row is a bootstrap row or a committed
    // one, so an unfiltered read is correct for everyone and the fast
    // path is untouched. Asserted through behaviour: the same statements
    // answer identically before and after a DDL transaction opens and
    // resolves.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE base (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO base VALUES (5)").substr(0, 8), "INSERTED");
    const std::vector<std::string> before = Rows(s, "SELECT id, v FROM base");

    Session ddl;
    ASSERT_EQ(Run(ddl, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(ddl, "CREATE TABLE other (id int64, v int64)").substr(0, 7), "CREATED");
    // While that is open, `base` still resolves for everyone: filtering is
    // on, and a committed relation passes it.
    EXPECT_EQ(Rows(s, "SELECT id, v FROM base"), before);
    ASSERT_EQ(Run(ddl, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // And afterwards, with nothing in flight again.
    EXPECT_EQ(Rows(s, "SELECT id, v FROM base"), before);
}

TEST_F(TxnSessionTest, ARolledBackDdlDoesNotSurviveInTheCatalogCache) {
    // **The hole DT3c's decision opens.** A rollback retires the catalog
    // rows through the transaction manager's compensation - the catalog
    // is never told, so it never drops its cached facts. Any unfiltered
    // read taken while the DDL was open therefore leaves an entry that
    // outlives the rows it describes, and once the transaction resolves
    // `ViewFor` goes back to the fast path and serves it.
    //
    // `SHOW TABLES` is the reachable spelling: it lists through
    // `ListTables`, which DT3c did not thread, so it both leaks the
    // uncommitted relation *and* caches it.
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "CREATE TABLE ghost (id int64, v int64)").substr(0, 7), "CREATED");

    (void)Run(b, "SHOW TABLES");  // fills the cache while the DDL is open
    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // The relation is gone from the pages. Nothing may still report it.
    EXPECT_EQ(Run(b, "SHOW TABLES").find("ghost"), std::string::npos)
        << "a rolled-back relation is still listed, from the cache";
    EXPECT_EQ(Run(b, "DESCRIBE ghost").rfind("ERR", 0), 0u)
        << "a rolled-back relation still resolves, from the cache";
}

}  // namespace
}  // namespace kds::server
