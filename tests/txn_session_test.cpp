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

// docs/spec/txn.md sections 10-5, 10-6, 10-7 and 10-8, end to end: two sessions
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

// ---- DELETE (docs/spec/txn.md sections 4.3, 6) --------------------------------

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
    // What `docs/spec/txn.md` §7 said was a known limitation until 2026-08-15:
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

TEST_F(TxnSessionTest, EveryRouteIntoARelationAgreesItIsInvisible) {
    // The closing-out of DT3c's list: each of these is a separate route
    // into "does this relation exist", and a single one that answered
    // differently would be the leak.
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "CREATE TABLE hidden (id int64, v int64)").substr(0, 7), "CREATED");

    for (const char* sql : {"DESCRIBE hidden",
                            "SELECT id, v FROM hidden",
                            "INSERT INTO hidden VALUES (1)",
                            "UPDATE hidden SET v = 1",
                            "DELETE FROM hidden WHERE id = 1",
                            "ALTER TABLE hidden RENAME TO shown",
                            "DROP TABLE hidden"}) {
        EXPECT_EQ(Run(b, sql).rfind("ERR", 0), 0u) << "leaked through: " << sql;
    }
    EXPECT_EQ(Run(b, "SHOW TABLES").find("hidden"), std::string::npos);

    // The creator reaches it by the same routes.
    EXPECT_EQ(Run(a, "DESCRIBE hidden").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run(a, "INSERT INTO hidden VALUES (5)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run(a, "UPDATE hidden SET v = 6"), "UPDATED 1");
    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");
}

TEST_F(TxnSessionTest, ASecondCreateOfTheSameNameIsRefusedWhileTheFirstIsOpen) {
    // Spec §6's open decision, in its conservative half: the duplicate
    // check is deliberately unfiltered, so the second create is refused
    // rather than producing two rows claiming one name. Pinned because it
    // is a decision, and a later change to filter that check would flip
    // this silently.
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "CREATE TABLE contested (id int64, v int64)").substr(0, 7), "CREATED");

    EXPECT_EQ(Run(b, "CREATE TABLE contested (id int64, v int64)").substr(0, 6), "EXISTS")
        << "two transactions both created a relation of the same name";

    // And after the first rolls back, the name is free.
    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Run(b, "CREATE TABLE contested (id int64, v int64)").substr(0, 7), "CREATED");
}

// ---- DT5: DROP TABLE is atomic (not isolated - spec §5a) ---------------

TEST_F(TxnSessionTest, ARolledBackDropTableRestoresTheRelationAndItsRows) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE keep (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO keep VALUES (11)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "INSERT INTO keep VALUES (22)").substr(0, 8), "INSERTED");

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP TABLE keep").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // The relation is back, by name and by schema...
    EXPECT_EQ(Run(s, "DESCRIBE keep").rfind("ERR", 0), std::string::npos)
        << "a rolled-back DROP did not restore the relation";
    // ...and so are its rows, which were never touched: a drop retires
    // catalog rows, not data pages.
    EXPECT_EQ(Rows(s, "SELECT id, v FROM keep"),
              (std::vector<std::string>{"1,11", "2,22"}));
}

TEST_F(TxnSessionTest, ACommittedDropTableStaysDropped) {
    // The guard against over-compensating: a committed drop must not be
    // undone, and its delete-marked rows must read as gone to everyone.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE doomed (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP TABLE doomed").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    EXPECT_EQ(Run(s, "DESCRIBE doomed").rfind("ERR", 0), 0u);
    Session other;
    EXPECT_EQ(Run(other, "DESCRIBE doomed").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run(other, "SHOW TABLES").find("doomed"), std::string::npos);

    // And the name is free again - the tombstone retype is what makes
    // that true, and it survived the commit.
    EXPECT_EQ(Run(other, "CREATE TABLE doomed (id int64, v int64)").substr(0, 7), "CREATED");
}

TEST_F(TxnSessionTest, AnAutocommitDropStillRetiresAndIsUnaffected) {
    // The path that always existed: no transaction, so the dependents are
    // retired outright exactly as before, and nothing is registered.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE plain (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "DROP TABLE plain").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run(s, "DESCRIBE plain").rfind("ERR", 0), 0u);

    // A later unrelated rollback cannot resurrect it.
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Run(s, "DESCRIBE plain").rfind("ERR", 0), 0u);
}

// ---- Indexes close the milestone's v1 scope (spec §5) ------------------

TEST_F(TxnSessionTest, ARolledBackCreateIndexLeavesNoIndex) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    {
        const std::string reply = Run(s, "CREATE INDEX by_owner ON t (owner)");
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    EXPECT_EQ(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos)
        << "a rolled-back CREATE INDEX left its catalog row behind";
    // The name is free again, which is the property a half-failed
    // migration needs.
    EXPECT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0),
              std::string::npos);
}

// **The test this replaces asserted a refusal, and the one before that
// asserted isolation.** DT5 shipped `DROP INDEX` as isolated on the
// strength of `SHOW INDEXES` filtering; `InitTableAccess` reads the index
// list unfiltered, so maintenance took the delete-mark the moment it was
// written and a rollback restored an index missing every row written
// meanwhile. It was refused rather than answered wrongly, and DT9 closes
// it at the read instead: an unfiltered catalog read counts a mark only
// once its deleter is no longer in flight.
//
// So this is the wrong-result scenario itself, run forwards.
TEST_F(TxnSessionTest, ARolledBackDropIndexLeavesTheIndexWholeIncludingTheWindowsWrites) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    {
        const std::string reply = Run(a, "CREATE INDEX by_owner ON t (owner)");
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    ASSERT_EQ(Run(a, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    {
        const std::string dropped = Run(a, "DROP INDEX by_owner");
        ASSERT_EQ(dropped.rfind("ERR", 0), std::string::npos) << dropped;
    }

    // The window. Another session writes a row while the drop is open; its
    // index entry is the one the old behaviour skipped.
    ASSERT_EQ(Run(b, "INSERT INTO t VALUES (20)").substr(0, 8), "INSERTED");

    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");

    EXPECT_NE(Run(a, "SHOW INDEXES").find("by_owner"), std::string::npos)
        << "a rolled-back DROP INDEX did not restore the index";

    // **The control this test needs to mean anything** (the pattern
    // `index_contract_test.cpp` calls "the control every equivalence suite
    // needs"): the SELECT below proves DT9 only while it actually probes
    // the index. If the planner ever stops choosing it, the query returns
    // the row from a scan and the assertion passes having tested nothing.
    EXPECT_NE(Run(a, "ANALYZE SELECT id, owner FROM t WHERE owner = 20").find("IndexProbe"),
              std::string::npos)
        << "this test no longer reaches the index, so it no longer tests DT9";

    // Reached through the restored index. Zero rows here is the wrong
    // answer the refusal existed to prevent.
    const std::vector<std::string> found = Rows(a, "SELECT id, owner FROM t WHERE owner = 20");
    ASSERT_EQ(found.size(), 1u) << "the index lost the row written while the drop was open";
    EXPECT_NE(found[0].find("20"), std::string::npos) << found[0];

    // And the row that predates the window is still there, so the arm did
    // not simply stop honouring marks.
    EXPECT_EQ(Rows(a, "SELECT id, owner FROM t WHERE owner = 10").size(), 1u);
}

// DT9 moved when a delete-mark starts counting, and therefore moved when
// the catalog's cache stops being true. A cache filled during an open
// `DROP INDEX` holds the index **deliberately** - that is what keeps
// maintenance writing entries a rollback would need - so the commit has
// to drop it. Before DT9 only a rollback invalidated, on the reasoning
// that "a commit leaves the rows in place"; that reasoning is what DT9
// retired.
TEST_F(TxnSessionTest, ACommittedDdlTransactionInvalidatesTheCatalogCache) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(a, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    // Another session fills the cache mid-flight, with the index in it.
    ASSERT_EQ(Run(b, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    const std::uint64_t before = boot_->catalog.catalog_version();
    ASSERT_EQ(Run(a, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_GT(boot_->catalog.catalog_version(), before)
        << "a committed DDL transaction left the cache holding a dropped index";
}

// The committed half of the same rule: once the drop commits, the mark
// counts, and the index is gone by every route.
// A refusal DT9 made reachable from a client for the first time: with one
// drop open, the index row is still there to be found, so the second drop
// matches a row it must not act on. Before DT9 the answer was "no index
// named ..."; without this it became "no sys.indexes row for this
// index_oid" - a system row's name, and no byte position, both against
// the rules every other refusal here follows.
TEST_F(TxnSessionTest, ASecondDropOfAnIndexAlreadyBeingDroppedIsRefusedInTheUsersTerms) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(a, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);

    for (Session* s : {&b, &a}) {  // another session, and the dropper itself
        const std::string refused = Run(*s, "DROP INDEX by_owner");
        EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
        EXPECT_NE(refused.find("by_owner"), std::string::npos) << refused;
        EXPECT_NE(refused.find("has not committed"), std::string::npos) << refused;
        EXPECT_NE(refused.find("byte "), std::string::npos) << refused;
        EXPECT_EQ(refused.find("sys.indexes"), std::string::npos)
            << "the refusal names a system row instead of the user's index: " << refused;
    }

    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_NE(Run(a, "SHOW INDEXES").find("by_owner"), std::string::npos);
}

// DT9 closed a corruption nobody had a test for. `CREATE INDEX` of a name
// whose drop is open used to be allowed - the marked row was invisible to
// the duplicate check - and the dropper's rollback then left two live
// sys.indexes rows claiming one name. That is exactly what §6 refuses for
// tables.
TEST_F(TxnSessionTest, ACreateIsRefusedWhileThatIndexNamesDropIsOpen) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(a, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);

    EXPECT_EQ(Run(b, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), 0u)
        << "a second session took an index name a pending drop can still restore";

    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");
    // Exactly one row claims the name, which is what the refusal bought.
    const std::string shown = Run(a, "SHOW INDEXES");
    EXPECT_NE(shown.find("by_owner"), std::string::npos);
    EXPECT_EQ(shown.find("by_owner", shown.find("by_owner") + 1), std::string::npos) << shown;
}

// DT10 (spec §5c). A committed transactional drop leaves its marked rows
// on the page whenever the resolution-time purge (§5d) could not take
// them - here because a leased reader still held the horizon, the same
// state a crash or a shutdown with a parked statement leaves behind - so
// the next mount inherits marks whose deleter it cannot ask about, and
// whose id the unlogged ceiling may even have reissued. Finalizing at
// mount deletes that question, and does so unconditionally: no reader
// exists at mount, so the horizon is nobody's business there.
TEST_F(TxnSessionTest, TheMountSweepRetiresTheMarksACommittedDropLeftBehind) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    // The reader that keeps §5d's resolution-time purge off the marks.
    auto view = mgr_->MintReadView(txn::kNoTrxId);
    ASSERT_TRUE(view.ok());
    auto lease = mgr_->RegisterReader(view.value());
    ASSERT_TRUE(lease.ok());

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    // The mark is on the page - that is what makes the sweep necessary,
    // and asserting the count proves the test is looking at a real one.
    auto first = boot_->catalog.FinalizeDeleteMarksAtMount();
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_GE(first.value(), 1u) << "a committed transactional drop left no mark to finalize";

    // Idempotent: the second mount over the same pages has nothing to do.
    // This is the property that keeps an ordinary restart free.
    auto second = boot_->catalog.FinalizeDeleteMarksAtMount();
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value(), 0u);

    // And the sweep changed no answer: the index was already gone.
    EXPECT_EQ(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos);
    EXPECT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);
}

// The other half: a clean instance that never dropped anything has no
// marks, so the sweep is free and says so.
TEST_F(TxnSessionTest, TheMountSweepFindsNothingOnAnInstanceThatDroppedNothing) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    auto swept = boot_->catalog.FinalizeDeleteMarksAtMount();
    ASSERT_TRUE(swept.ok()) << swept.status().message();
    EXPECT_EQ(swept.value(), 0u);

    // Nothing it could reach was disturbed.
    EXPECT_NE(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos);
    EXPECT_EQ(Rows(s, "SELECT id, owner FROM t WHERE owner = 10").size(), 1u);
}

TEST_F(TxnSessionTest, ACommittedDropIndexInsideATransactionRemovesTheIndex) {
    Session a;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    {
        const std::string reply = Run(a, "CREATE INDEX by_owner ON t (owner)");
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    ASSERT_EQ(Run(a, "INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(a, "COMMIT").substr(0, 6), "COMMIT");

    EXPECT_EQ(Run(a, "SHOW INDEXES").find("by_owner"), std::string::npos);
    // The rows are the index's business, not the relation's: the answer is
    // the same one, found by a scan.
    EXPECT_EQ(Rows(a, "SELECT id, owner FROM t WHERE owner = 10").size(), 1u);
    // And the name is free, which only a mark that counts can make true.
    EXPECT_EQ(Run(a, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);
}

TEST_F(TxnSessionTest, AutocommitIndexDdlIsUnchanged) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    {
        const std::string reply = Run(s, "CREATE INDEX by_owner ON t (owner)");
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    ASSERT_EQ(Run(s, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);

    // A later unrelated rollback resurrects neither.
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos);
}

// The other half of §6's refusal, and the one the duplicate check cannot
// make on its own. `DROP TABLE` frees the name for everyone the moment it
// runs (§5a's in-place retype), but the drop can still roll back - so a
// create that took the name meanwhile left **two live rows claiming it**
// once the rollback rewrote the tombstone back to a table.
TEST_F(TxnSessionTest, ACreateIsRefusedWhileAnotherTransactionsDropOfTheNameIsOpen) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE shared (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP TABLE shared").rfind("ERR", 0), std::string::npos);

    EXPECT_EQ(Run(b, "CREATE TABLE shared (id int64, v int64)").rfind("ERR", 0), 0u)
        << "a second session took a name a pending drop can still restore";

    ASSERT_EQ(Run(a, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // Exactly one relation is named `shared`, and it is the original.
    const std::string tables = Run(b, "SHOW TABLES");
    std::size_t claims = 0;
    for (std::size_t at = tables.find("shared"); at != std::string::npos;
         at = tables.find("shared", at + 1)) {
        ++claims;
    }
    EXPECT_EQ(claims, 1u) << "two rows claim one name: " << tables;

    // And once the drop resolves, the name is free again.
    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP TABLE shared").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(a, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Run(b, "CREATE TABLE shared (id int64, v int64)").substr(0, 7), "CREATED");
}

TEST_F(TxnSessionTest, ATransactionMayDropAndRecreateOneNameItself) {
    // The refusal above is about *another* transaction's pending drop. A
    // transaction sees its own, so the migration shape still works - and
    // rolls back whole.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE mine (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP TABLE mine").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run(s, "CREATE TABLE mine (id int64, w int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    Session other;
    EXPECT_NE(Run(other, "DESCRIBE mine").find("w"), std::string::npos);
}

// ---- §5d: the horizon-gated purge at DDL resolution --------------------
//
// docs/workplan-reader-registration.md D5. DDL resolution is the only
// event that creates or settles a catalog delete-mark, so EndDdlScope
// attempts the purge there; the read horizon is what proves no live view
// can still see a marked row, and a reader that holds the horizon simply
// holds the marks - to the next resolution, or to the mount sweep above.

TEST_F(TxnSessionTest, ACommittedDropsMarksArePurgedAtItsOwnResolution) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    // The commit's own resolution took the marks: nothing is left for a
    // manual pass, nothing for the next mount, and SHOW META counted it.
    auto again = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value(), 0u);
    auto mount = boot_->catalog.FinalizeDeleteMarksAtMount();
    ASSERT_TRUE(mount.ok()) << mount.status().message();
    EXPECT_EQ(mount.value(), 0u) << "the resolution-time purge left marks for the mount sweep";
    const std::string meta = Run(s, "SHOW META");
    EXPECT_NE(meta.find("catalog_marks_purged="), std::string::npos) << meta;
    EXPECT_EQ(meta.find("catalog_marks_purged=0"), std::string::npos) << meta;

    EXPECT_EQ(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos);
}

TEST_F(TxnSessionTest, ALeasedReaderHoldsTheMarksAndItsReleaseFreesThem) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    // An autocommit reader from before the drop - what a parked
    // session-side statement or a shipped stage holds.
    auto view = mgr_->MintReadView(txn::kNoTrxId);
    ASSERT_TRUE(view.ok());
    auto lease = mgr_->RegisterReader(view.value());
    ASSERT_TRUE(lease.ok());

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    // Held: the lease's view cannot see the dropper, so the horizon sits
    // below it and the purge must leave the marks.
    auto held = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(held.ok()) << held.status().message();
    EXPECT_EQ(held.value(), 0u) << "the purge retired marks a leased reader could still need";

    lease.value().Release();
    auto freed = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(freed.ok()) << freed.status().message();
    EXPECT_GE(freed.value(), 1u) << "releasing the last older reader did not free the marks";

    // And done: a second pass has nothing left.
    auto empty = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty.value(), 0u);
}

TEST_F(TxnSessionTest, AnOlderOpenTransactionHoldsTheMarksUntilItResolves) {
    Session a;
    Session b;
    ASSERT_EQ(Run(a, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(a, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    // b's transaction starts first, so its id sits below the dropper's and
    // bounds the horizon - live_ is its registration, no lease involved.
    ASSERT_EQ(Run(b, "BEGIN").substr(0, 5), "BEGIN");

    ASSERT_EQ(Run(a, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(a, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(a, "COMMIT").substr(0, 6), "COMMIT");

    auto held = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(held.ok()) << held.status().message();
    EXPECT_EQ(held.value(), 0u) << "the purge retired marks an open older transaction could see";

    // b resolves without DDL, so nothing triggers automatically - the
    // marks wait for the next resolution or the mount, by design.
    ASSERT_EQ(Run(b, "COMMIT").substr(0, 6), "COMMIT");
    auto freed = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(freed.ok()) << freed.status().message();
    EXPECT_GE(freed.value(), 1u);
}

// The undo purge's SHOW META pair (docs/inflight/in-progress/workplan-undo-purge.md UP3):
// present whenever a manager is, and live pages count at least the page
// a write created.
TEST_F(TxnSessionTest, ShowMetaCarriesTheUndoPurgeCounters) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES (1)").substr(0, 8), "INSERTED");

    const std::string meta = Run(s, "SHOW META");
    EXPECT_NE(meta.find("undo_pages_live="), std::string::npos) << meta;
    EXPECT_NE(meta.find("undo_pages_recycled="), std::string::npos) << meta;
    EXPECT_EQ(meta.find("undo_pages_live=0"), std::string::npos)
        << "an INSERT wrote an undo record, so at least one page must be live: " << meta;
}

TEST_F(TxnSessionTest, ARolledBackDropLeavesNothingForThePurge) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DROP INDEX by_owner").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // The rollback cleared its own marks synchronously; the purge finds a
    // clean page and the index is whole.
    auto swept = boot_->catalog.PurgeSettledDeleteMarks();
    ASSERT_TRUE(swept.ok()) << swept.status().message();
    EXPECT_EQ(swept.value(), 0u);
    EXPECT_NE(Run(s, "SHOW INDEXES").find("by_owner"), std::string::npos);
}

// ---- NULL storage's surface refusals (docs/workplan-null.md NU3) --------

TEST_F(TxnSessionTest, ANullablePrimaryKeyIsRefusedWithItsByte) {
    Session s;
    const std::string out = Run(s, "CREATE TABLE t (id int64 NULL, v int64)");
    ASSERT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("Keystone"), std::string::npos) << out;
    EXPECT_NE(out.find("byte"), std::string::npos) << out;
}

TEST_F(TxnSessionTest, AnIndexOnANullableColumnIsRefusedByName) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, v int64 NULL) BTREE").substr(0, 7), "CREATED");
    const std::string out = Run(s, "CREATE INDEX by_v ON t (v)");
    ASSERT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("nullable"), std::string::npos) << out;
    // D2 promises the byte position, delivered by index_ddl's resolve -
    // the layer that still holds the column token's offset.
    EXPECT_NE(out.find("byte"), std::string::npos) << out;
    // Covered columns are refused by the same rule: an entry encodes
    // their values with no bitmap of its own.
    ASSERT_EQ(Run(s, "CREATE TABLE w (id int64, k int64, v int64 NULL) BTREE").substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run(s, "CREATE INDEX by_k ON w (k) COVERING (v)").rfind("ERR", 0), 0u);
    // And the NOT NULL twin indexes fine - the refusal is the column's,
    // not the feature's.
    ASSERT_EQ(Run(s, "CREATE TABLE u (id int64, v int64) BTREE").substr(0, 7), "CREATED");
    EXPECT_EQ(Run(s, "CREATE INDEX by_uv ON u (v)").rfind("ERR", 0), std::string::npos);
}

// ---- C4: every route takes the statement boundary ---------------------

TEST_F(TxnSessionTest, EveryRouteSeesARelationCommittedSinceTheTransactionBegan) {
    // **A READ COMMITTED violation, and a violation of DT3c's own rule
    // that every route agrees.** `ViewFor` reads the transaction's view,
    // but only the routes reaching `SnapshotFor`/`BeginWrite` re-minted
    // it at the statement boundary - so `DESCRIBE`, `SHOW TABLES` and
    // friends resolved under whatever view the transaction last held and
    // could miss a relation committed since it began. `SELECT` saw it,
    // `DESCRIBE` did not, in the same transaction.
    //
    // Only reachable while some transaction holds uncommitted DDL, since
    // that is when `ViewFor` filters at all - which is why `holder` is
    // here.
    Session reader;
    Session holder;
    Session writer;

    ASSERT_EQ(Run(reader, "BEGIN").substr(0, 5), "BEGIN");
    // Turn filtering on and keep it on.
    ASSERT_EQ(Run(holder, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(holder, "CREATE TABLE unrelated (id int64, v int64)").substr(0, 7),
              "CREATED");

    // A relation committed after `reader` began. Under READ COMMITTED the
    // next statement in `reader` must see it - by every route.
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "CREATE TABLE later (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");

    EXPECT_EQ(Run(reader, "DESCRIBE later").rfind("ERR", 0), std::string::npos)
        << "DESCRIBE resolved under a stale view";
    EXPECT_NE(Run(reader, "SHOW TABLES").find("later"), std::string::npos)
        << "SHOW TABLES resolved under a stale view";
    EXPECT_EQ(Run(reader, "SELECT id, v FROM later").rfind("ERR", 0), std::string::npos)
        << "SELECT and DESCRIBE disagree, which is the property DT3c claims";

    ASSERT_EQ(Run(reader, "ROLLBACK").substr(0, 8), "ROLLBACK");
    ASSERT_EQ(Run(holder, "ROLLBACK").substr(0, 8), "ROLLBACK");
}

TEST_F(TxnSessionTest, RepeatableReadStillHoldsOneViewAcrossTheseRoutes) {
    // The latch must not turn RR into RC: `StartStatement` is a no-op
    // under REPEATABLE READ, so taking the boundary more often changes
    // nothing there. Pinned because the fix touches the one branch that
    // *is* the difference between the levels.
    Session reader;
    Session holder;
    Session writer;
    ASSERT_EQ(Run(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(holder, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(holder, "CREATE TABLE unrelated (id int64, v int64)").substr(0, 7),
              "CREATED");

    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "CREATE TABLE after_rr (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");

    // RR held its view from BEGIN, so the new relation is not there yet.
    EXPECT_EQ(Run(reader, "DESCRIBE after_rr").rfind("ERR", 0), 0u)
        << "REPEATABLE READ saw a relation committed after it began";
    ASSERT_EQ(Run(reader, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Run(reader, "DESCRIBE after_rr").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run(holder, "ROLLBACK").substr(0, 8), "ROLLBACK");
}

}  // namespace
}  // namespace kds::server
