// `BEGIN ... READ ONLY` (work order CS, `docs/spec/txn.md` §1): an explicit
// transaction that is a read view and a reader lease held across
// statements - no id drawn, no record appended, no in-flight slot taken -
// and a `READ WRITE` transaction left exactly as it was (CS-R4).
//
// On the two-core rig so a cell can commit on one core while the read-only
// transaction holds its view on the other.

#include "two_core_rig.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"
#include "kds/txn/manager.hpp"

namespace kds::server {
namespace {

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

class ReadOnlyTxnTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_TRUE(StartsWith(D(0).Dispatch("CREATE TABLE t (id int64, v int64) BTREE").response,
                               "CREATED"));
        ASSERT_TRUE(StartsWith(D(0).Dispatch("INSERT INTO t VALUES (1, 1)").response, "INSERTED"));
    }

    CommandDispatcher& D(std::uint32_t core) { return rig_->core(core).dispatcher(); }
    std::string Run(std::uint32_t core, Session& s, const std::string& sql) {
        return D(core).Dispatch(sql, &s).response;
    }

    // What a writer's `BEGIN` spends, read on `core`: the id cursor, the
    // log's append point, the in-flight table and the live set.
    struct Spent {
        std::uint64_t next_id;
        wal::Lsn appended;
        std::uint32_t in_flight;
        std::size_t active;
        bool operator==(const Spent&) const = default;
    };
    Spent SpentOn(std::uint32_t core) {
        return {rig_->core(core).trx_ids().peek(), rig_->wal().appended_lsn(),
                rig_->visibility().InFlightCount(core),
                rig_->core(core).transactions().ActiveCount()};
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(ReadOnlyTxnTest, AReadOnlyTransactionDrawsNoIdAppendsNoRecordAndTakesNoSlot) {
    Session s;
    const Spent before = SpentOn(1);
    EXPECT_EQ(Run(1, s, "BEGIN READ ONLY"),
              "BEGIN trx_id=none isolation=read committed access=read only");
    EXPECT_TRUE(s.in_explicit_txn());
    EXPECT_TRUE(s.in_read_only_txn());
    EXPECT_EQ(s.transaction(), nullptr);
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    EXPECT_TRUE(SpentOn(1) == before) << "a read-only transaction spent a writer's machinery";
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_FALSE(s.in_explicit_txn());
    EXPECT_FALSE(s.in_read_only_txn());
    EXPECT_TRUE(SpentOn(1) == before) << "its COMMIT appended or published something";

    EXPECT_EQ(Run(1, s, "BEGIN TRANSACTION READ ONLY"),
              "BEGIN trx_id=none isolation=read committed access=read only");
    EXPECT_EQ(Run(1, s, "ROLLBACK"), "ROLLBACK trx_id=none");
    EXPECT_TRUE(SpentOn(1) == before);
}

TEST_F(ReadOnlyTxnTest, AReadWriteTransactionIsUnchangedByteForByte) {
    // CS-R4: the same id, the same records, the same reply.
    Session s;
    const Spent before = SpentOn(0);
    const std::string id = std::to_string(before.next_id);
    EXPECT_EQ(Run(0, s, "BEGIN"),
              "BEGIN trx_id=" + id + " isolation=read committed durability=group");
    EXPECT_NE(rig_->wal().appended_lsn(), before.appended) << "TXN_BEGIN was not appended";
    EXPECT_EQ(rig_->visibility().InFlightCount(0), before.in_flight + 1);
    EXPECT_EQ(Run(0, s, "COMMIT"), "COMMIT trx_id=" + id);

    // `READ WRITE` written is the default spelled out.
    Session w;
    const std::string next = std::to_string(SpentOn(0).next_id);
    EXPECT_EQ(Run(0, w, "START TRANSACTION READ WRITE"),
              "BEGIN trx_id=" + next + " isolation=read committed durability=group");
    EXPECT_FALSE(w.in_read_only_txn());
    EXPECT_NE(w.transaction(), nullptr);
    EXPECT_EQ(Run(0, w, "ROLLBACK"), "ROLLBACK trx_id=" + next);
}

TEST_F(ReadOnlyTxnTest, TheAccessClauseIsOrderFreeBesideTheOtherTwo) {
    Session s;
    EXPECT_EQ(Run(1, s, "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ"),
              "BEGIN trx_id=none isolation=repeatable read access=read only");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_EQ(Run(1, s, "BEGIN ISOLATION LEVEL READ COMMITTED READ ONLY"),
              "BEGIN trx_id=none isolation=read committed access=read only");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    // CS-Q10 (a): a class binds nothing a read-only commit acknowledges.
    EXPECT_EQ(Run(1, s, "BEGIN DURABILITY strict READ ONLY ISOLATION LEVEL repeatable_read"),
              "BEGIN trx_id=none isolation=repeatable read access=read only");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");

    EXPECT_NE(Run(1, s, "BEGIN READ SOMETHING").find("expected ONLY or WRITE after READ"),
              std::string::npos);
    EXPECT_FALSE(s.in_explicit_txn());
    EXPECT_NE(Run(1, s, "BEGIN READ").find("expected ONLY or WRITE after READ"),
              std::string::npos);
    EXPECT_FALSE(s.in_explicit_txn());
}

TEST_F(ReadOnlyTxnTest, RepeatableReadHoldsItsViewAcrossAnotherCoresCommit) {
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ"), "BEGIN"));
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    ASSERT_EQ(D(0).Dispatch("UPDATE t SET v = 2 WHERE id = 1").response, "UPDATED 1");
    ASSERT_TRUE(StartsWith(D(0).Dispatch("INSERT INTO t VALUES (2, 7)").response, "INSERTED"));
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1")
        << "the held view moved with another core's commit";
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 2"), "v")
        << "a row inserted after the view was taken is visible to it";
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n2");
}

TEST_F(ReadOnlyTxnTest, ItsLeaseHoldsTheHorizonAgainstAPurgeOnAnotherCore) {
    // CS-R2, `[quiet-wrong]`, across cores: core 1's read-only view is
    // published to the instance only through its lease's slot
    // (`PublishCoreBounds`), so a `PURGE` on core 0 that judged the deleter
    // resolved for every reader would retire a row core 1 is still owed.
    ASSERT_TRUE(StartsWith(D(0).Dispatch("INSERT INTO t VALUES (5, 50)").response, "INSERTED"));
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 5"), "v\\n50");
    ASSERT_EQ(D(0).Dispatch("DELETE FROM t WHERE id = 5").response, "DELETED 1");
    // A second statement boundary, so the hold is across statements.
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");

    const std::string refused = D(0).Dispatch("PURGE FROM t WHERE id = 5").response;
    EXPECT_NE(refused, "PURGED 1") << "core 0 purged under core 1's read-only view";
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 5"), "v\\n50")
        << "the read-only view lost the row it was owed";
    ASSERT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_EQ(D(0).Dispatch("PURGE FROM t WHERE id = 5").response, "PURGED 1")
        << "the released lease still held the horizon";
}

TEST_F(ReadOnlyTxnTest, ReadCommittedTakesAFreshViewAtEveryStatement) {
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    ASSERT_EQ(D(0).Dispatch("UPDATE t SET v = 2 WHERE id = 1").response, "UPDATED 1");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n2");
    ASSERT_EQ(D(0).Dispatch("UPDATE t SET v = 3 WHERE id = 1").response, "UPDATED 1");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n3");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
}

TEST_F(ReadOnlyTxnTest, TheLeaseRegistryIsTheOnlyCapAndItsRefusalNamesTheHolders) {
    // CS-R6, CS-Q5 (a): every read-only transaction holds one of the core's
    // registry slots for its whole life, shared with suspended walks.
    std::vector<Session> held(txn::kMaxRegisteredReaders);
    for (Session& s : held) ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    Session over;
    const std::string refused = Run(1, over, "BEGIN READ ONLY");
    EXPECT_NE(refused.find("registered readers"), std::string::npos) << refused;
    EXPECT_NE(refused.find("READ ONLY transactions"), std::string::npos) << refused;
    EXPECT_FALSE(over.in_explicit_txn());
    // The other core's registry is its own.
    Session elsewhere;
    EXPECT_TRUE(StartsWith(Run(0, elsewhere, "BEGIN READ ONLY"), "BEGIN"));
    EXPECT_EQ(Run(0, elsewhere, "COMMIT"), "COMMIT trx_id=none");

    ASSERT_EQ(Run(1, held.front(), "COMMIT"), "COMMIT trx_id=none");
    EXPECT_TRUE(StartsWith(Run(1, over, "BEGIN READ ONLY"), "BEGIN"));
    EXPECT_EQ(Run(1, over, "COMMIT"), "COMMIT trx_id=none");
    for (std::size_t i = 1; i < held.size(); ++i) Run(1, held[i], "ROLLBACK");
}

TEST_F(ReadOnlyTxnTest, AReadonlyConnectionBeginsReadOnlyAndMayNotBeginReadWrite) {
    // CS-Q3 (a).
    Session s;
    s.set_role(Role::kReadOnly);
    const Spent before = SpentOn(1);
    EXPECT_EQ(Run(1, s, "BEGIN"), "BEGIN trx_id=none isolation=read committed access=read only");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_TRUE(SpentOn(1) == before);
    EXPECT_EQ(Run(1, s, "BEGIN READ WRITE"),
              "ERR permission: BEGIN READ WRITE needs readwrite; this connection is readonly");
    EXPECT_FALSE(s.in_explicit_txn());
    // The frame's spelling (`C_TXN_BEGIN{durability}`) is read-only too.
    EXPECT_EQ(Run(1, s, "BEGIN DURABILITY strict"),
              "BEGIN trx_id=none isolation=read committed access=read only");
    EXPECT_EQ(Run(1, s, "ROLLBACK"), "ROLLBACK trx_id=none");
}

// CS-R1 (`[quiet-wrong]`): every write route a statement head can reach,
// refused inside a `READ ONLY` transaction before it touches anything - no
// WAL byte appended, no page dirtied - and the transaction left as it was.
TEST_F(ReadOnlyTxnTest, EveryWriteRouteIsRefusedAtItsHeadAndTouchesNothing) {
    ASSERT_TRUE(StartsWith(D(1).Dispatch("CREATE INDEX t_v ON t (v)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(D(1).Dispatch("CREATE TABLE u (id int64, w int64) BTREE").response,
                           "CREATED"));
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");

    const std::vector<std::string> writes = {
        "INSERT INTO t VALUES (5, 5)",
        "insert into t values (6, 6)",
        "INSERT INTO t SELECT id + 100, v FROM t",
        "INSERT INTO t VALUES (1, 9) ON CONFLICT DO NOTHING",
        "INSERT INTO t VALUES (1, 9) ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v",
        "UPDATE t SET v = 9 WHERE id = 1",
        "UPDATE t SET v = u.w FROM u WHERE t.id = u.id",
        "DELETE FROM t WHERE id = 1",
        "DELETE FROM t USING u WHERE t.id = u.id",
        "PURGE FROM t WHERE id = 1",
        "PURGE DROPPED COLUMNS FROM t",
        "CREATE TABLE z (id int64) BTREE",
        "CREATE TABLE z2",
        "DROP TABLE u",
        "CREATE INDEX t_w ON t (v)",
        "CREATE UNIQUE INDEX t_u ON t (v)",
        "DROP INDEX t_v",
        "REINDEX INDEX t_v",
        "CREATE NAMESPACE n1",
        "DROP NAMESPACE public",
        "ALTER TABLE u RENAME TO u2",
        "ALTER TABLE u RENAME COLUMN w TO w2",
        "ALTER TABLE u ADD COLUMN x int64 NULL",
        "CREATE CABIN ON t (v)",
        "DROP CABIN ON t (v)",
        "CREATE ASSERTION a1 ON t CHECK COUNT(*) < 100",
        "DROP ASSERTION a1",
        "CREATE TRANSITION tr ON t(v) ON INSERT (1) ON UPDATE (1 => 2)",
        "DROP TRANSITION tr",
        "WAYSTONE ON SELECT v FROM t WHERE id = 1",
        "WAYSTONE OFF SELECT v FROM t WHERE id = 1",
    };
    for (const std::string& sql : writes) {
        const wal::Lsn appended = rig_->wal().appended_lsn();
        const std::vector<PageId> dirty = rig_->store().DirtyPageIds();
        const DispatchOutcome out = D(1).Dispatch(sql, &s);
        EXPECT_EQ(out.status.code(), StatusCode::kReadOnlyTransaction)
            << sql << ": " << out.response;
        EXPECT_TRUE(StartsWith(out.response, "ERR READ_ONLY_TRANSACTION retryable=0 "))
            << sql << ": " << out.response;
        EXPECT_NE(out.response.find("(byte 0)"), std::string::npos) << out.response;
        EXPECT_EQ(rig_->wal().appended_lsn(), appended) << sql << " appended to the log";
        EXPECT_EQ(rig_->store().DirtyPageIds(), dirty) << sql << " dirtied a page";
        // CS-Q7 (a): not poisoned.
        EXPECT_FALSE(s.failed()) << sql;
        EXPECT_TRUE(s.in_read_only_txn()) << sql;
    }

    // The held view is the one the transaction began with: another core's
    // commit after the refusals is not in it.
    ASSERT_EQ(D(0).Dispatch("UPDATE t SET v = 2 WHERE id = 1").response, "UPDATED 1");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    // CS-Q8 (a): this core's counters, in `sys.meta`'s `txn` section.
    EXPECT_EQ(Run(1, s, "SELECT value FROM sys.meta WHERE field = 'read_only_write_refusals'"),
              "value\\n" + std::to_string(writes.size()));
    EXPECT_EQ(Run(1, s, "SELECT value FROM sys.meta WHERE field = 'read_only_begins'"),
              "value\\n1");
    EXPECT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    // Nothing a refusal named happened.
    EXPECT_EQ(D(1).Dispatch("SELECT v FROM t WHERE id = 5").response, "v");
    EXPECT_TRUE(StartsWith(D(1).Dispatch("SELECT w FROM u").response, "w"));
}

TEST_F(ReadOnlyTxnTest, ReadsAndSessionStatementsAreAdmitted) {
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    for (const char* sql :
         {"SELECT * FROM t", "select v from t where id = 1", "ANALYZE SELECT v FROM t WHERE id = 1",
          "SHOW TABLES", "DESCRIBE t", "SELECT * FROM sys.tables", "USE public",
          "SET PRIORITY analytic", "PING"}) {
        const std::string reply = Run(1, s, sql);
        EXPECT_FALSE(StartsWith(reply, "ERR")) << sql << ": " << reply;
    }
    // A write behind ANALYZE's prefix is the write it is (DU-S3): refused
    // by the transaction's mode, which leaves the transaction as it was.
    const std::string analyzed = Run(1, s, "ANALYZE UPDATE t SET v = 9 WHERE id = 1");
    EXPECT_TRUE(StartsWith(analyzed, "ERR")) << analyzed;
    EXPECT_NE(analyzed.find("READ ONLY"), std::string::npos) << analyzed;
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    EXPECT_EQ(Run(1, s, "ROLLBACK"), "ROLLBACK trx_id=none");
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");

    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    // The level and the class still bind the next transaction, not this one.
    EXPECT_TRUE(StartsWith(Run(1, s, "SET ISOLATION LEVEL repeatable read"), "ERR"));
    EXPECT_TRUE(StartsWith(Run(1, s, "BEGIN"), "ERR a transaction is already open"));
    EXPECT_EQ(Run(1, s, "ROLLBACK"), "ROLLBACK trx_id=none");
}

}  // namespace
}  // namespace kds::server
