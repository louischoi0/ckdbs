// `SELECT ... FOR UPDATE`, the locking read (work order DC,
// `instructions/v3.2.0/workorder-dc-select-for-update.md`; `docs/spec/txn.md`
// §5, `docs/spec/parser-v2.md` I11a).
//
// On the lock family's one-core fixture, driven through the dispatcher's
// served path, so a cell can park a statement on a held row and observe the
// park.

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/base/status.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/role.hpp"
#include "kds/server/session.hpp"
#include "kds/wire/error_registry.hpp"
#include "lock_family_fixture.hpp"

namespace kds::server {
namespace {

// A session's sink that counts what reaches it. A re-run does not rewind a
// session's sink, so what a parked run let out is still counted afterwards.
class CountingSink final : public ResultSink {
public:
    Status Describe(std::vector<wire::FieldDescription>) override {
        ++described;
        return Status::OK();
    }
    Status EncodeProjectedRow(std::span<const exec::ColumnRef>, std::span<const std::uint32_t>,
                              const exec::ChainFrame&, std::string& out) override {
        out.clear();
        return Status::OK();
    }
    Status EncodeValueRow(std::span<const std::uint32_t>, std::span<const parser::AstValue>,
                          std::string& out) override {
        out.clear();
        return Status::OK();
    }
    Status Emit(std::string_view) override {
        ++rows;
        return Status::OK();
    }

    int described = 0;
    int rows = 0;
};

class LockingReadTest : public LockFamilyTest {
protected:
    void SetUp() override {
        LockFamilyTest::SetUp();
        ASSERT_EQ(Local("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)").rfind("INSERTED", 0), 0u);
        ASSERT_EQ(Local("CREATE TABLE u (id int64, t_id int64)").rfind("CREATED", 0), 0u);
    }

    DispatchOutcome In(Session& session, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &session);
    }
    void Begin(Session& session, const std::string& how = "BEGIN") {
        ASSERT_EQ(In(session, how).response.rfind("BEGIN", 0), 0u);
    }

    // `sql` is refused with `code`, naming the byte of its last `FOR` and
    // saying `why` - the words that tell this refusal from any other at the
    // same byte.
    void ExpectRefusedAtFor(Session& session, const std::string& sql, StatusCode code,
                            std::string_view why) {
        const DispatchOutcome out = In(session, sql);
        ASSERT_EQ(out.response.rfind("ERR", 0), 0u) << sql << " answered " << out.response;
        const Status refused = StatusFromErrorReply(out.response);
        EXPECT_EQ(refused.code(), code) << sql << ": " << out.response;
        const std::string at = "(byte " + std::to_string(sql.rfind("FOR")) + ")";
        EXPECT_NE(out.response.find(at), std::string::npos)
            << sql << " should name " << at << ": " << out.response;
        EXPECT_NE(out.response.find(why), std::string::npos)
            << sql << " should say '" << why << "': " << out.response;
    }
};

// ---- DC-S1: the gates -----------------------------------------------------

TEST_F(LockingReadTest, AutocommitIsRefusedAtTheByteOfFor) {
    // DC-Q7 (a): the claim would end with the statement.
    Session s;
    ExpectRefusedAtFor(s, "SELECT * FROM t WHERE id = 1 FOR UPDATE", StatusCode::kNotImplemented,
                       "outside a transaction");
    EXPECT_FALSE(s.in_explicit_txn());
    // And with no session of the caller's own.
    EXPECT_NE(Local("SELECT * FROM t FOR UPDATE").find("outside a transaction"),
              std::string::npos);
}

TEST_F(LockingReadTest, AReadOnlyTransactionRefusesItUnpoisonedAndCounted) {
    // DC-R7: a write for access mode, though both gates above the handler
    // read `SELECT`. CS-Q7's refusal: the transaction is as it was.
    Session s;
    Begin(s, "BEGIN READ ONLY");
    const std::string count = "SELECT value FROM sys.meta WHERE field = 'read_only_write_refusals'";
    const std::string before = In(s, count).response;
    const std::string sql = "SELECT * FROM t WHERE id = 1 FOR UPDATE";
    ExpectRefusedAtFor(s, sql, StatusCode::kReadOnlyTransaction, "READ ONLY transaction");
    EXPECT_FALSE(s.failed()) << "a refused write must not fail a READ ONLY transaction";
    EXPECT_NE(In(s, count).response, before) << "the refusal was not counted";
    EXPECT_EQ(In(s, "SELECT v FROM t WHERE id = 1").response, "v\\n10");
    EXPECT_EQ(In(s, "COMMIT").response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockingReadTest, TheReadonlyRoleCannotLock) {
    // The role gate admits `SELECT` to `readonly`; the clause needs
    // `readwrite`, asked after the parse.
    Session s;
    s.set_role(Role::kReadOnly);
    const std::string sql = "SELECT * FROM t WHERE id = 1 FOR UPDATE";
    const DispatchOutcome out = In(s, sql);
    EXPECT_EQ(out.response.rfind("ERR permission: SELECT ... FOR UPDATE needs readwrite (byte " +
                                     std::to_string(sql.find("FOR")) + ")",
                                 0),
              0u)
        << out.response;
    // Its plain read is admitted as before.
    EXPECT_EQ(In(s, "SELECT v FROM t WHERE id = 1").response, "v\\n10");
    // And inside the transaction it can open - `READ ONLY` - the role is
    // still the first refusal.
    Begin(s);
    EXPECT_TRUE(s.in_read_only_txn());
    EXPECT_EQ(In(s, "SELECT * FROM t FOR UPDATE").response.rfind("ERR permission", 0), 0u);
    EXPECT_FALSE(s.failed());
    Session rw;
    rw.set_role(Role::kReadWrite);
    Begin(rw);
    EXPECT_EQ(In(rw, "SELECT * FROM t WHERE id = 1 FOR UPDATE").response.find("permission"),
              std::string::npos);
}

// ---- DC-S1: the shapes (DC-Q5 (a)) ----------------------------------------

TEST_F(LockingReadTest, EveryUnbuiltShapeIsRefusedAtTheByteOfForAndFailsNothing) {
    Session s;
    Begin(s);
    const std::pair<const char*, const char*> shapes[] = {
        {"SELECT t.v FROM t JOIN u ON t.id = u.t_id FOR UPDATE", "over a join"},
        {"SELECT COUNT(*) FROM t FOR UPDATE", "over an aggregate"},
        {"SELECT v, COUNT(*) FROM t GROUP BY v FOR UPDATE", "over an aggregate"},
        {"SELECT v FROM t GROUP BY v FOR UPDATE", "over an aggregate"},
        {"SELECT * FROM t ORDER BY id FOR UPDATE", "with ORDER BY"},
        {"SELECT * FROM t LIMIT 1 FOR UPDATE", "with LIMIT"},
        {"SELECT * FROM t OFFSET 1 FOR UPDATE", "with OFFSET"},
        // The query layer's forms: refused by its grammar, by name.
        {"SELECT v + 1 FROM t FOR UPDATE", "the query layer answers"},
        {"SELECT DISTINCT v FROM t FOR UPDATE", "the query layer answers"},
        {"SELECT * FROM t LEFT JOIN u ON t.id = u.t_id FOR UPDATE", "the query layer answers"},
        {"SELECT v FROM t UNION SELECT t_id FROM u FOR UPDATE", "the query layer answers"},
        {"SELECT * FROM (SELECT v FROM t FOR UPDATE) AS d", "inside a subquery"},
        // A clause inside a subquery.
        {"SELECT * FROM t WHERE id IN (SELECT t_id FROM u FOR UPDATE)", "inside a subquery"},
    };
    for (const auto& [sql, why] : shapes) {
        ExpectRefusedAtFor(s, sql, StatusCode::kNotImplemented, why);
        EXPECT_FALSE(s.failed()) << sql << " failed the transaction (DC-Q11)";
    }
    // A catalog view has no row to claim: the architecture's refusal.
    ExpectRefusedAtFor(s, "SELECT * FROM sys.tables FOR UPDATE", StatusCode::kUnsupported,
                       "catalog view");
    EXPECT_FALSE(s.failed());
    // A word after an accepted clause is the trailing token it always was,
    // answered by the grammar that read the clause (not the query layer's).
    const std::string trailing = "SELECT * FROM t FOR UPDATE extra";
    const std::string refused = In(s, trailing).response;
    EXPECT_EQ(StatusFromErrorReply(refused).code(), StatusCode::kInvalidArgument) << refused;
    EXPECT_NE(refused.find("(byte " + std::to_string(trailing.find("extra")) + ")"),
              std::string::npos)
        << refused;
    EXPECT_FALSE(s.failed());
    // Every other strength and option, by name (DC-Q2, DC-Q6).
    for (const char* sql :
         {"SELECT * FROM t FOR SHARE", "SELECT * FROM t FOR KEY SHARE",
          "SELECT * FROM t FOR NO KEY UPDATE", "SELECT * FROM t FOR UPDATE NOWAIT",
          "SELECT * FROM t FOR UPDATE SKIP LOCKED", "SELECT * FROM t FOR UPDATE OF t"}) {
        EXPECT_EQ(StatusFromErrorReply(In(s, sql).response).code(), StatusCode::kNotImplemented)
            << sql;
        EXPECT_FALSE(s.failed()) << sql;
    }
    // The transaction went on through all of it.
    EXPECT_EQ(In(s, "SELECT v FROM t WHERE id = 2").response, "v\\n20");
    EXPECT_EQ(In(s, "COMMIT").response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockingReadTest, AQueryLayerFormIsNotAnsweredWithTheClauseDropped) {
    // DC-R2. `SELECT v + 1 FROM t` is the query layer's and answers rows;
    // with the clause it must not.
    Session s;
    Begin(s);
    EXPECT_EQ(In(s, "SELECT v + 1 FROM t WHERE id = 1").response.rfind("ERR", 0),
              std::string::npos);
    const DispatchOutcome out = In(s, "SELECT v + 1 FROM t WHERE id = 1 FOR UPDATE");
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("FOR UPDATE"), std::string::npos)
        << "the refusal must be about the clause, not about the expression the query layer "
           "answers: "
        << out.response;
}

TEST_F(LockingReadTest, NoPathThatDoesNotClaimTakesALockingStatement) {
    // DC-R2: `ANALYZE`, a Waystone opt-in and a plan each compile a chain,
    // and the compile refuses a statement carrying the clause.
    Session s;
    const std::string locking = "SELECT * FROM t WHERE id = 1 FOR UPDATE";
    const std::string plans = Local("SELECT name FROM sys.plans");
    // Each with the words of its own refusal: the route's for `ANALYZE`,
    // the compile's for the three that compile an example.
    const std::pair<std::string, const char*> routes[] = {
        {"ANALYZE " + locking, "ANALYZE of"},
        {"WAYSTONE ON " + locking, "compiles to no step chain"},
        {"CREATE PLAN p FOR SELECT t.v FROM t JOIN u ON t.id = u.t_id FOR UPDATE AS SEARCHED",
         "compiles to no step chain"},
        {"ADVISE PLAN FOR SELECT t.v FROM t JOIN u ON t.id = u.t_id FOR UPDATE",
         "compiles to no step chain"},
    };
    for (const auto& [sql, why] : routes) {
        const DispatchOutcome out = In(s, sql);
        ASSERT_EQ(out.response.rfind("ERR", 0), 0u) << sql << " answered " << out.response;
        EXPECT_EQ(StatusFromErrorReply(out.response).code(), StatusCode::kNotImplemented)
            << sql << ": " << out.response;
        EXPECT_NE(out.response.find(why), std::string::npos) << sql << ": " << out.response;
    }
    EXPECT_EQ(Local("SELECT name FROM sys.plans"), plans) << "a plan was created";
    // Inside a transaction `ANALYZE` is refused the same way.
    Begin(s);
    EXPECT_NE(In(s, "ANALYZE " + locking).response.find("ANALYZE of"), std::string::npos);
    EXPECT_FALSE(s.failed());
}

TEST_F(LockingReadTest, AnInsertsSourceAndACteAreRefusedByName) {
    Session s;
    Begin(s);
    const std::string insert = "INSERT INTO u SELECT id, v FROM t FOR UPDATE";
    const DispatchOutcome out = In(s, insert);
    EXPECT_EQ(StatusFromErrorReply(out.response).code(), StatusCode::kNotImplemented)
        << out.response;
    EXPECT_NE(out.response.find("(byte " + std::to_string(insert.find("FOR")) + ")"),
              std::string::npos)
        << out.response;
    In(s, "ROLLBACK");
    // `WITH` is a CTE, refused as one whatever follows it.
    const std::string cte =
        In(s, "WITH w AS (SELECT * FROM t) SELECT * FROM w FOR UPDATE").response;
    EXPECT_EQ(StatusFromErrorReply(cte).code(), StatusCode::kNotImplemented) << cte;
    EXPECT_NE(cte.find("WITH"), std::string::npos) << cte;
}

TEST_F(LockingReadTest, ALockingStatementIsAShapeOfItsOwn) {
    // DC-R9: the clause's words are hashed as shape, so the locking
    // statement is counted and keyed apart from the plain one.
    const auto plain = parser::FingerprintOf("SELECT * FROM t WHERE id = 1");
    const auto locking = parser::FingerprintOf("SELECT * FROM t WHERE id = 1 FOR UPDATE");
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(locking.has_value());
    EXPECT_NE(plain->fetch_id, locking->fetch_id);
    EXPECT_NE(plain->pattern_id, locking->pattern_id);
    EXPECT_EQ(plain->arg_hash, locking->arg_hash) << "the literal is the same argument";
}

// ---- DC-S2: the rows ------------------------------------------------------

TEST_F(LockingReadTest, ItsRowsAreThePlainStatementsByteForByte) {
    // In a quiet instance the clause changes nothing a client reads: the
    // description and every row are the plain statement's, whichever arm
    // reaches them - the tuple key, a list on it, a window, the walk, the
    // collection pass of a WHERE holding a subquery.
    ASSERT_EQ(Local("INSERT INTO u VALUES (1, 1), (2, 3)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE h (a int64, b varchar) HEAP").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO h VALUES (1, 'one'), (2, 'two'), (3, 'three')").rfind("INSERTED", 0),
              0u);
    // A value past the inline cell, so a row's spill is resolved on the way out.
    ASSERT_EQ(Local("CREATE TABLE w (id int64, s varchar(16))").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO w VALUES (1, '" + std::string(600, 'x') + "'), (2, 'short')")
                  .rfind("INSERTED", 0),
              0u);
    Session s;
    Begin(s);
    for (const char* sql : {
             "SELECT * FROM t",
             "SELECT v FROM t WHERE id = 2",
             "SELECT v, id FROM t WHERE id IN (1, 3)",
             "SELECT * FROM t WHERE id IN (3, 1, 3, 99)",
             "SELECT * FROM t WHERE v > 10",
             "SELECT * FROM t WHERE id BETWEEN 2 AND 3",
             "SELECT * FROM t WHERE id > 1 AND v < 30",
             "SELECT * FROM t WHERE v = 20 OR v = 30",
             "SELECT t.v FROM t WHERE t.id = 1",
             "SELECT x.v, x.id FROM t AS x WHERE x.v >= 20",
             "SELECT * FROM t WHERE id = 99",
             "SELECT * FROM t WHERE v = 99",
             "SELECT * FROM t WHERE id IN (SELECT t_id FROM u)",
             "SELECT v FROM t WHERE id = 1 AND v = 10",
             "SELECT * FROM h",
             "SELECT b FROM h WHERE a = 2",
             "SELECT * FROM h WHERE b = 'three'",
             "SELECT * FROM w",
             "SELECT s FROM w WHERE id = 1",
             "SELECT * FROM public.t WHERE id = 3",
         }) {
        const std::string plain = In(s, sql).response;
        ASSERT_NE(plain.rfind("ERR", 0), 0u) << sql << ": " << plain;
        const DispatchOutcome locking = In(s, std::string(sql) + " FOR UPDATE");
        EXPECT_EQ(locking.response, plain) << sql;
        EXPECT_TRUE(locking.status.ok()) << sql << ": " << locking.status.message();
        EXPECT_FALSE(s.failed()) << sql;
    }
    EXPECT_EQ(In(s, "SELECT v FROM t WHERE id = 2 FOR UPDATE").rows_returned, 1u);
    EXPECT_EQ(In(s, "SELECT * FROM t FOR UPDATE").rows_returned, 3u);
    EXPECT_EQ(In(s, "COMMIT").response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockingReadTest, ARefusalBeforeAnyUnitIsAskedFailsNothing) {
    // DC-Q11 (a): the resolve and the compile are refused as a SELECT's are.
    Session s;
    Begin(s);
    for (const char* sql :
         {"SELECT nope FROM t FOR UPDATE", "SELECT * FROM t WHERE nope = 1 FOR UPDATE",
          "SELECT * FROM missing FOR UPDATE", "SELECT z.v FROM t FOR UPDATE",
          "SELECT * FROM sys.no_such_view FOR UPDATE", "SELECT * FROM t WHERE v = 'text' FOR UPDATE"}) {
        const DispatchOutcome out = In(s, sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << sql << " answered " << out.response;
        EXPECT_FALSE(s.failed()) << sql << " failed the transaction: " << out.response;
    }
    // The refusals are the plain statement's own, word for word.
    EXPECT_EQ(In(s, "SELECT nope FROM t FOR UPDATE").response, In(s, "SELECT nope FROM t").response);
    EXPECT_EQ(In(s, "SELECT * FROM t WHERE id = 1 FOR UPDATE").response, "id,v\\n1,10");
    EXPECT_EQ(In(s, "COMMIT").response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockingReadTest, ItAppendsNoRecordAndLeavesNoTrailEntry) {
    // DC-R5, DC-R6. (No page dirtied is `locking_read_rig_test.cpp`'s, on a
    // store that tracks dirty frames.)
    Session s;
    Begin(s);
    const wal::Lsn appended = wal_->appended_lsn();
    for (const char* sql :
         {"SELECT * FROM t WHERE id = 1 FOR UPDATE", "SELECT * FROM t WHERE id IN (1, 2) FOR UPDATE",
          "SELECT * FROM t WHERE v > 0 FOR UPDATE", "SELECT * FROM t FOR UPDATE"}) {
        ASSERT_NE(In(s, sql).response.rfind("ERR", 0), 0u) << sql;
        EXPECT_EQ(wal_->appended_lsn(), appended) << sql << " appended to the log";
    }
    EXPECT_EQ(s.transaction()->trail().size(), 0u) << "a locking read left a trail entry";
    EXPECT_EQ(In(s, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
}

// ---- DC-S2: the claim -----------------------------------------------------

TEST_F(LockingReadTest, AnUpdateADeleteAndALockingReadEachWaitForTheHolderAtBothDecides) {
    struct Waiter {
        const char* sql;    // of the row whose key is appended
        const char* reply;  // once the holder has decided
        bool in_txn;
    };
    const Waiter waiters[] = {{"UPDATE t SET v = 1 WHERE id = ", "UPDATED 1", false},
                              {"DELETE FROM t WHERE id = ", "DELETED 1", false},
                              {"SELECT v FROM t WHERE id = ", "v\\n100", true}};
    std::uint64_t key = 10;
    for (const char* decide : {"COMMIT", "ROLLBACK"}) {
        for (const Waiter& waiter : waiters) {
            const std::string k = std::to_string(++key);
            const std::string what = std::string(waiter.sql) + k + " behind a " + decide;
            ASSERT_EQ(Local("INSERT INTO t (id, v) VALUES (" + k + ", 100)").rfind("INSERTED", 0), 0u);
            Session holder;
            Begin(holder);
            ASSERT_EQ(In(holder, "SELECT v FROM t WHERE id = " + k + " FOR UPDATE").response,
                      "v\\n100");

            Session other;
            if (waiter.in_txn) Begin(other);
            Started w =
                Start(waiter.sql + k + (waiter.in_txn ? " FOR UPDATE" : ""), other);
            Pump();
            ASSERT_FALSE(*w.done) << what << " passed the claim: " << w.out->response;
            // A row the read did not return is nobody's.
            EXPECT_EQ(Local("UPDATE t SET v = 21 WHERE id = 2"), "UPDATED 1") << what;

            ASSERT_EQ(In(holder, decide).response.rfind(decide, 0), 0u);
            Pump();
            ASSERT_TRUE(*w.done) << what << " never ran";
            EXPECT_EQ(w.out->response, waiter.reply) << what;
            if (waiter.in_txn) ASSERT_EQ(In(other, "COMMIT").response.rfind("COMMIT", 0), 0u);
        }
    }
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}

TEST_F(LockingReadTest, ASecondLockingReadWaitsAndThenReturnsWhatTheHolderLeft) {
    // The clause's whole point: the second reader is not handed the row
    // until the first has decided, and it is handed the decided value.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");

    Session second;
    Begin(second);
    Started r = Start("SELECT v FROM t WHERE id = 1 FOR UPDATE", second);
    Pump();
    ASSERT_FALSE(*r.done) << "two transactions hold one row's claim: " << r.out->response;
    EXPECT_FALSE(second.failed());

    ASSERT_EQ(In(holder, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1")
        << "a transaction's own UPDATE of a row it claimed asks nothing new";
    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*r.done);
    EXPECT_EQ(r.out->response, "v\\n11") << "the re-run read under a fresh view";
    EXPECT_FALSE(second.failed());
    EXPECT_EQ(In(second, "COMMIT").response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockingReadTest, AWalkedLockingReadWaitsAtAHeldRowAndReturnsEveryRowOnce) {
    // The walk stops at the held row with nothing sent (DC-R4), and its
    // re-run returns each row once, the rows claimed before the park among
    // them.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "UPDATE t SET v = 21 WHERE id = 2").response, "UPDATED 1");

    Session reader;
    Begin(reader);
    Started r = Start("SELECT * FROM t WHERE v > 0 FOR UPDATE", reader);
    Pump();
    ASSERT_FALSE(*r.done) << r.out->response;

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*r.done);
    EXPECT_EQ(r.out->response, "id,v\\n1,10\\n2,21\\n3,30");
    EXPECT_EQ(r.out->rows_returned, 3u);
}

TEST_F(LockingReadTest, NoRowAndNoDescriptionLeaveBeforeEveryRowIsClaimed) {
    // DC-R4, on a session's own sink - the one a re-run does not rewind, so
    // a row the parked run let out would be counted twice here and would
    // have left for good on the wire. Row 1 is claimed before the walk
    // meets the held row 2, and still nothing has left.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "UPDATE t SET v = 21 WHERE id = 2").response, "UPDATED 1");

    Session reader;
    Begin(reader);
    CountingSink sink;
    reader.set_result_sink(&sink);
    Started r = Start("SELECT * FROM t WHERE v > 0 FOR UPDATE", reader);
    Pump();
    ASSERT_FALSE(*r.done) << r.out->response;
    EXPECT_EQ(sink.described, 0) << "the description left before the claim pass ended";
    EXPECT_EQ(sink.rows, 0) << "a row left before every row was claimed";

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*r.done);
    reader.set_result_sink(nullptr);
    EXPECT_TRUE(r.out->status.ok()) << r.out->status.message();
    EXPECT_EQ(sink.described, 1);
    EXPECT_EQ(sink.rows, 3) << "each row once";
    EXPECT_EQ(r.out->rows_returned, 3u);
}

TEST_F(LockingReadTest, ItReturnsTheTransactionsOwnVersionOfARowItWrote) {
    // DC-R1's other admitted writer: this transaction. Its update's
    // after-image, its own insert, and not the row it deleted.
    Session s;
    Begin(s);
    ASSERT_EQ(In(s, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");
    ASSERT_EQ(In(s, "INSERT INTO t (id, v) VALUES (5, 50)").response.rfind("INSERTED", 0), 0u);
    ASSERT_EQ(In(s, "DELETE FROM t WHERE id = 2").response, "DELETED 1");
    EXPECT_EQ(In(s, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n11");
    EXPECT_EQ(In(s, "SELECT * FROM t WHERE id = 2 FOR UPDATE").response,
              In(s, "SELECT * FROM t WHERE id = 2").response);
    EXPECT_EQ(In(s, "SELECT * FROM t FOR UPDATE").response, "id,v\\n1,11\\n3,30\\n5,50");
    EXPECT_FALSE(s.failed());
    EXPECT_EQ(In(s, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
}

TEST_F(LockingReadTest, AClaimOutlivesALaterFailedStatementUntilTheRollback) {
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");
    ASSERT_EQ(In(holder, "UPDATE t SET nope = 1 WHERE id = 3").response.rfind("ERR", 0), 0u);
    ASSERT_TRUE(holder.failed());

    Session writer;
    Started w = Start("UPDATE t SET v = 11 WHERE id = 1", writer);
    Pump();
    EXPECT_FALSE(*w.done) << "a failed transaction gave its claim back before its ROLLBACK";
    ASSERT_EQ(In(holder, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done);
    EXPECT_EQ(w.out->response, "UPDATED 1");
}

TEST_F(LockingReadTest, WithTheClauseNoIncrementIsLostAndWithoutItOneIs) {
    // The motive's cell: two READ COMMITTED sessions each read a counter,
    // add one and write it back.
    const auto value = [&] { return Local("SELECT v FROM t WHERE id = 1"); };

    // Without the clause both read 10, and the second write overwrites the
    // first commit with nothing refused.
    {
        Session a, b;
        Begin(a);
        Begin(b);
        ASSERT_EQ(In(a, "SELECT v FROM t WHERE id = 1").response, "v\\n10");
        ASSERT_EQ(In(b, "SELECT v FROM t WHERE id = 1").response, "v\\n10");
        ASSERT_EQ(In(a, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");
        ASSERT_EQ(In(a, "COMMIT").response.rfind("COMMIT", 0), 0u);
        ASSERT_EQ(In(b, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");
        ASSERT_EQ(In(b, "COMMIT").response.rfind("COMMIT", 0), 0u);
        EXPECT_EQ(value(), "v\\n11") << "two increments of 10 lost one";
    }
    ASSERT_EQ(Local("UPDATE t SET v = 10 WHERE id = 1"), "UPDATED 1");

    // With it the second reader is held until the first commits, and reads 11.
    {
        Session a, b;
        Begin(a);
        Begin(b);
        ASSERT_EQ(In(a, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");
        Started rb = Start("SELECT v FROM t WHERE id = 1 FOR UPDATE", b);
        Pump();
        ASSERT_FALSE(*rb.done);
        ASSERT_EQ(In(a, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");
        ASSERT_EQ(In(a, "COMMIT").response.rfind("COMMIT", 0), 0u);
        Pump();
        ASSERT_TRUE(*rb.done);
        ASSERT_EQ(rb.out->response, "v\\n11");
        ASSERT_EQ(In(b, "UPDATE t SET v = 12 WHERE id = 1").response, "UPDATED 1");
        ASSERT_EQ(In(b, "COMMIT").response.rfind("COMMIT", 0), 0u);
        EXPECT_EQ(value(), "v\\n12") << "an increment was lost under the clause";
    }
}

TEST_F(LockingReadTest, AnInsertIntoADeclaredWindowWaitsForTheHolder) {
    // DC-Q8 (a): a bound on the tuple key declares its window, as an
    // UPDATE's does, so a key that appears inside it waits.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT * FROM t WHERE id BETWEEN 2 AND 100 FOR UPDATE").response,
              "id,v\\n2,20\\n3,30");

    Session writer;
    Started inside = Start("INSERT INTO t (id, v) VALUES (50, 5)", writer);
    Pump();
    ASSERT_FALSE(*inside.done) << "a key appeared inside a held window: " << inside.out->response;
    // Outside the window nothing is fenced.
    EXPECT_EQ(Local("INSERT INTO t (id, v) VALUES (200, 5)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(Local("UPDATE t SET v = 11 WHERE id = 1"), "UPDATED 1");

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*inside.done);
    EXPECT_EQ(inside.out->response.rfind("INSERTED", 0), 0u) << inside.out->response;
}

TEST_F(LockingReadTest, AForeignKeyChildPassesTheClaimAndTheHoldersDeleteIsThenRefused) {
    // DC-Q1 (a), `[quiet-wrong]`: the claim is `U`, which a child's share
    // passes - PostgreSQL's FOR NO KEY UPDATE, not its FOR UPDATE. A client
    // that locks a parent to keep children out is not served by it.
    ASSERT_EQ(Local("CREATE TABLE p (id int64, n int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE").rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO p VALUES (1, 0)").rfind("INSERTED", 0), 0u);

    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT * FROM p WHERE id = 1 FOR UPDATE").response, "id,n\\n1,0");

    Session child;
    const DispatchOutcome inserted = RunAsync("INSERT INTO c VALUES (1, 1)", child);
    EXPECT_EQ(inserted.response.rfind("INSERTED", 0), 0u)
        << "a child's share did not pass the claim: " << inserted.response;

    // The holder's own DELETE asks `X` now, and the committed child refuses it.
    const DispatchOutcome deleted = RunAsync("DELETE FROM p WHERE id = 1", holder);
    EXPECT_EQ(deleted.response.rfind("ERR", 0), 0u) << deleted.response;
    EXPECT_NE(deleted.response.find("referenced"), std::string::npos) << deleted.response;
    In(holder, "ROLLBACK");
}

TEST_F(LockingReadTest, TwoSessionsClaimingTwoRowsInOppositeOrdersEndWithOneRefusedNamingDeadlock) {
    Session a, b;
    Begin(a);
    Begin(b);
    ASSERT_EQ(In(a, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");
    ASSERT_EQ(In(b, "SELECT v FROM t WHERE id = 2 FOR UPDATE").response, "v\\n20");

    Started wa = Start("SELECT v FROM t WHERE id = 2 FOR UPDATE", a);
    Pump();
    ASSERT_FALSE(*wa.done) << wa.out->response;

    Started wb = Start("SELECT v FROM t WHERE id = 1 FOR UPDATE", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "the cycle was not detected";
    EXPECT_EQ(StatusFromErrorReply(wb.out->response).code(), StatusCode::kTxnConflict);
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos) << wb.out->response;
    EXPECT_TRUE(b.failed()) << "the victim's transaction is failed, and still holds row 2";
    EXPECT_FALSE(*wa.done);

    ASSERT_EQ(In(b, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*wa.done);
    EXPECT_EQ(wa.out->response, "v\\n20");
    EXPECT_EQ(In(a, "COMMIT").response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}

// ---- DC-S2: the version (DC-R1, DC-Q3 (a)) ---------------------------------

TEST_F(LockingReadTest, ARepeatableReadReaderWaitsOutAHolderThatWroteNoVersion) {
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");

    Session reader;
    Begin(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ");
    Started r = Start("SELECT v FROM t WHERE id = 1 FOR UPDATE", reader);
    Pump();
    ASSERT_FALSE(*r.done) << r.out->response;

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*r.done);
    EXPECT_EQ(r.out->response, "v\\n10");
    EXPECT_FALSE(reader.failed());
}

TEST_F(LockingReadTest, ARepeatableReadReaderIsRefusedBehindAHolderThatWroteTheRow) {
    // The holder's commit is one its view cannot see, so the wait would be
    // futile: refused at once, first-updater-wins.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");

    Session reader;
    Begin(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ");
    const DispatchOutcome refused = RunAsync("SELECT v FROM t WHERE id = 1 FOR UPDATE", reader);
    EXPECT_EQ(StatusFromErrorReply(refused.response).code(), StatusCode::kTxnConflict)
        << refused.response;
    EXPECT_TRUE(reader.failed());
    In(holder, "ROLLBACK");
}

TEST_F(LockingReadTest, ARowNewerThanTheViewIsRefusedNeverReturnedAndItsUnitIsHeldToTheRollback) {
    // DC-R1 and DC-Q3 (a)'s cost. The row's newest version committed after
    // the reader's view: returning the view's version under a held lock
    // would be the lost update the clause exists to prevent, so it is
    // refused - and the refusal fails the transaction with the row's unit,
    // borrowed before the verdict, kept until the ROLLBACK. READ COMMITTED
    // meets the same arm in the window between its view and its claim.
    Session reader;
    Begin(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ");
    ASSERT_EQ(In(reader, "SELECT v FROM t WHERE id = 2").response, "v\\n20");
    ASSERT_EQ(Local("UPDATE t SET v = 11 WHERE id = 1"), "UPDATED 1");

    const DispatchOutcome refused = RunAsync("SELECT v FROM t WHERE id = 1 FOR UPDATE", reader);
    EXPECT_EQ(StatusFromErrorReply(refused.response).code(), StatusCode::kTxnConflict)
        << "a version the view does not see was returned or skipped: " << refused.response;
    EXPECT_TRUE(reader.failed());
    EXPECT_EQ(In(reader, "COMMIT").response.rfind("ERR", 0), 0u) << "a failed transaction committed";

    Session writer;
    Started w = Start("UPDATE t SET v = 12 WHERE id = 1", writer);
    Pump();
    EXPECT_FALSE(*w.done) << "the refused read's unit was given back before its ROLLBACK";
    ASSERT_EQ(In(reader, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done);
    EXPECT_EQ(w.out->response, "UPDATED 1");
}

TEST_F(LockingReadTest, ARowAnUndecidedWriterMovedOffTheWhereIsWaitedForNotSkipped) {
    // DC-R1's "never skipped", by `Qualify`'s rule (F4): the version this
    // statement sees matches, the writer's does not, and the writer may
    // still roll back.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "UPDATE t SET v = 99 WHERE id = 2").response, "UPDATED 1");

    Session reader;
    Begin(reader);
    Started r = Start("SELECT * FROM t WHERE v = 20 FOR UPDATE", reader);
    Pump();
    ASSERT_FALSE(*r.done) << "the row was skipped behind an undecided writer: " << r.out->response;

    ASSERT_EQ(In(holder, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*r.done);
    EXPECT_EQ(r.out->response, "id,v\\n2,20");
}

// ---- DC-S2: what waits for the holder's relation IX ------------------------

TEST_F(LockingReadTest, ADropTableWaitsForTheHolder) {
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT * FROM u FOR UPDATE").response.rfind("ERR", 0), std::string::npos);

    Session ddl;
    Started drop = Start("DROP TABLE u", ddl);
    Pump();
    ASSERT_FALSE(*drop.done) << "a DDL's X passed a locking read's IX: " << drop.out->response;

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*drop.done);
    EXPECT_EQ(drop.out->response.rfind("DROPPED", 0), 0u) << drop.out->response;
}

TEST_F(LockingReadTest, AnIndexBuildWaitsForTheHolder) {
    // An online `CREATE INDEX` drains the relation's open writers before it
    // is served, and a locking read's `IX` makes its transaction one.
    Session holder;
    Begin(holder);
    ASSERT_EQ(In(holder, "SELECT v FROM t WHERE id = 1 FOR UPDATE").response, "v\\n10");

    Session ddl;
    Started build = Start("CREATE INDEX t_v ON t (v)", ddl);
    Pump();
    ASSERT_FALSE(*build.done) << "the build did not wait for the holder: " << build.out->response;

    ASSERT_EQ(In(holder, "COMMIT").response.rfind("COMMIT", 0), 0u);
    Pump(256);
    ASSERT_TRUE(*build.done);
    EXPECT_EQ(build.out->response.rfind("ERR", 0), std::string::npos) << build.out->response;
    EXPECT_EQ(Local("SELECT v FROM t WHERE v = 20"), "v\\n20");
}

// ---- DC-S2: the cap ---------------------------------------------------------

class LockingReadCapTest : public LockingReadTest {
protected:
    // The relation's `IX` and three units - what the fixture's three-row
    // INSERT takes - so a fourth unit is refused.
    std::size_t LockCap() const override { return 4; }
};

TEST_F(LockingReadCapTest, ARowUnitReadPastTheCapIsRefusedNamingItAndAWindowIsNot) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 40)").rfind("INSERTED", 0), 0u);
    Session s;
    Begin(s);
    // No WHERE declares the whole id space: one unit, whatever the rows.
    EXPECT_EQ(In(s, "SELECT * FROM t FOR UPDATE").rows_returned, 4u);
    ASSERT_EQ(In(s, "ROLLBACK").response.rfind("ROLLBACK", 0), 0u);

    // A predicate naming no key takes a unit a row, and four rows are one
    // unit past the cap. Nothing is returned, and the transaction is failed.
    Begin(s);
    const DispatchOutcome out = In(s, "SELECT * FROM t WHERE v > 0 FOR UPDATE");
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap));
    EXPECT_TRUE(s.failed());
    In(s, "ROLLBACK");
}

}  // namespace
}  // namespace kds::server
