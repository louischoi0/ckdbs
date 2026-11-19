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
#include "kds/server/role.hpp"
#include "kds/server/session.hpp"
#include "lock_family_fixture.hpp"

namespace kds::server {
namespace {

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

TEST_F(LockingReadTest, AWellFormedLockingReadPassesEveryGateAndIsNotBuiltYet) {
    // DC-S1's own end: the gates admit it, and nothing serves it yet.
    Session s;
    Begin(s);
    for (const char* sql : {"SELECT * FROM t WHERE id = 1 FOR UPDATE", "SELECT v FROM t FOR UPDATE",
                            "SELECT x.v FROM t AS x WHERE x.v > 10 FOR UPDATE"}) {
        ExpectRefusedAtFor(s, sql, StatusCode::kNotImplemented, "not built yet");
        EXPECT_FALSE(s.failed()) << sql;
    }
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

}  // namespace
}  // namespace kds::server
