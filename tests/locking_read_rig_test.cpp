// `SELECT ... FOR UPDATE` on the two-core rig (work order DC,
// `docs/spec/txn.md` §5): the cells that need a store tracking dirty frames
// and a Cabin store. The waits are `locking_read_test.cpp`'s, and the
// cross-core wake `row_wait_wake_rig_test.cpp`'s.

#include "two_core_rig.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

class LockingReadRigTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_TRUE(StartsWith(D(0).Dispatch("CREATE TABLE t (id int64, v int64) BTREE").response,
                               "CREATED"));
        ASSERT_TRUE(StartsWith(
            D(0).Dispatch("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)").response, "INSERTED"));
    }

    CommandDispatcher& D(std::uint32_t core) { return rig_->core(core).dispatcher(); }
    std::string Run(std::uint32_t core, Session& s, const std::string& sql) {
        return D(core).Dispatch(sql, &s).response;
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(LockingReadRigTest, NoPageIsDirtiedAndNoRecordIsAppended) {
    // DC-Q9 (a), DC-R5: the claim pass fetches its leaves for read, where
    // the write walk it is built on fetches them for writing.
    ASSERT_FALSE(StartsWith(D(0).Dispatch("SYNC").response, "ERR"));
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    const wal::Lsn appended = rig_->wal().appended_lsn();
    const std::vector<PageId> dirty = rig_->store().DirtyPageIds();
    for (const char* sql : {
             "SELECT * FROM t WHERE id = 1 FOR UPDATE",
             "SELECT * FROM t WHERE id IN (2, 3) FOR UPDATE",
             "SELECT * FROM t WHERE v > 10 FOR UPDATE",
             "SELECT * FROM t WHERE id BETWEEN 1 AND 2 FOR UPDATE",
             "SELECT * FROM t WHERE id IN (SELECT id FROM t WHERE v = 30) FOR UPDATE",
             "SELECT v FROM t FOR UPDATE",
         }) {
        const std::string reply = Run(1, s, sql);
        ASSERT_FALSE(StartsWith(reply, "ERR")) << sql << ": " << reply;
        EXPECT_EQ(rig_->wal().appended_lsn(), appended) << sql << " appended to the log";
        EXPECT_EQ(rig_->store().DirtyPageIds(), dirty) << sql << " dirtied a page";
    }
    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));

    // **A write walk that writes no row dirties no leaf either** (DT): it
    // looks at each leaf under a shared hold and takes for writing only one
    // it writes a row in.
    const std::vector<PageId> before = rig_->store().DirtyPageIds();
    ASSERT_EQ(D(1).Dispatch("UPDATE t SET v = 1 WHERE v = 999").response, "UPDATED 0");
    EXPECT_EQ(rig_->store().DirtyPageIds(), before) << "a walk that wrote no row dirtied a leaf";
    // The cell is not vacuous: a row written does dirty its leaf.
    ASSERT_EQ(D(1).Dispatch("UPDATE t SET v = v WHERE v > 10").response, "UPDATED 2");
    EXPECT_NE(rig_->store().DirtyPageIds(), before)
        << "a write that wrote rows dirtied nothing, so the checks above pinned nothing";
}

TEST_F(LockingReadRigTest, ACabinOverThePredicateDoesNotAnswerIt) {
    // DC-R2: a Cabin set finds a plain SELECT's rows; a locking read takes
    // the write's walk, returns the same rows and holds each one.
    const std::string created = D(0).Dispatch("CREATE CABIN ON t (v)").response;
    ASSERT_FALSE(StartsWith(created, "ERR")) << created;
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    const std::string plain = Run(1, s, "SELECT * FROM t WHERE v = 20");
    EXPECT_EQ(plain, "id,v\\n2,20");
    const std::string analyzed = Run(1, s, "ANALYZE SELECT * FROM t WHERE v = 20");
    ASSERT_NE(analyzed.find("CabinProbe"), std::string::npos)
        << "the plain statement is not a Cabin probe, so the cell compares nothing: " << analyzed;
    EXPECT_EQ(Run(1, s, "SELECT * FROM t WHERE v = 20 FOR UPDATE"), plain);

    // A dispatch with no reactor to park on names the holder instead of
    // waiting: the row is claimed.
    Session other;
    const std::string held = Run(0, other, "UPDATE t SET v = 21 WHERE id = 2");
    EXPECT_TRUE(StartsWith(held, "ERR TXN_CONFLICT")) << held;
    EXPECT_EQ(Run(0, other, "UPDATE t SET v = 31 WHERE id = 3"), "UPDATED 1");
    ASSERT_TRUE(StartsWith(Run(1, s, "COMMIT"), "COMMIT"));
    EXPECT_EQ(Run(0, other, "UPDATE t SET v = 21 WHERE id = 2"), "UPDATED 1");
}

TEST_F(LockingReadRigTest, AReadCommittedReadRefusedInItsWindowFailsItsTransactionAndKeepsTheUnit) {
    // DC-Q3 (a)'s window, made to open: a commit lands after the statement
    // took its view and before it claims the row. The read is refused - the
    // view's version under a held lock would be the lost update - the
    // refusal fails the transaction, and the unit, borrowed before the
    // verdict, is held until the ROLLBACK. The commit comes from the other
    // core's dispatcher, through the seam after the view.
    Session s;
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    bool landed = false;
    D(1).SetAfterViewForTest([&] {
        if (landed) return;
        landed = true;
        EXPECT_EQ(D(0).Dispatch("UPDATE t SET v = 11 WHERE id = 1").response, "UPDATED 1");
    });
    const DispatchOutcome refused = D(1).Dispatch("SELECT v FROM t WHERE id = 1 FOR UPDATE", &s);
    D(1).SetAfterViewForTest(nullptr);
    ASSERT_TRUE(landed);
    EXPECT_EQ(refused.status.code(), StatusCode::kTxnConflict) << refused.response;
    EXPECT_TRUE(StartsWith(refused.response, "ERR TXN_CONFLICT retryable=1 ")) << refused.response;
    EXPECT_TRUE(s.failed());
    EXPECT_TRUE(StartsWith(Run(1, s, "COMMIT"), "ERR"));

    Session other;
    const std::string held = Run(0, other, "UPDATE t SET v = 12 WHERE id = 1");
    EXPECT_TRUE(StartsWith(held, "ERR TXN_CONFLICT")) << "the unit was given back: " << held;
    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Run(0, other, "UPDATE t SET v = 12 WHERE id = 1"), "UPDATED 1");

    // The same statement, retried in a new transaction, reads what landed.
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    EXPECT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1 FOR UPDATE"), "v\\n12");
    ASSERT_TRUE(StartsWith(Run(1, s, "COMMIT"), "COMMIT"));
}

}  // namespace
}  // namespace kds::server
