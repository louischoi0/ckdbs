#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BU-S3: a subquery predicate under `OR` or `NOT`
// (instructions/v3.0.0/workorder-bu-basic-query-surface.md).
//
// Each one becomes a leaf of its boolean conjunct and a sub-chain of the step
// the conjunct is placed at, marked `in_tree` (step_chain.hpp). Three things
// can go wrong, and each has cells. **Hoisting**: an uncorrelated `EXISTS`
// under an `OR` is not a conjunct of the statement, so a false one must not
// empty it. **Three-valued logic** (BU-R4): `IN` over a set holding NULL is
// unknown, and `NOT` keeps it unknown. **Indexing**: a leaf names its
// sub-chain by its place in the step's list, beside the flat sub-chains and
// other trees' - a wrong index reads another subquery's answer.

namespace kds::server {
namespace {

class WhereBoolSubqueryTest : public ::testing::Test {
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

        // t: ids 1..6, k = id - 1, n nullable: NULL on ids 2 and 5.
        Ok("CREATE TABLE t (id int64, k int64, sym varchar, n int64 NULL) BTREE");
        Ok("INSERT INTO t VALUES (0, 'a', 1)");
        Ok("INSERT INTO t VALUES (1, 'b', NULL)");
        Ok("INSERT INTO t VALUES (2, 'a', 3)");
        Ok("INSERT INTO t VALUES (3, 'b', 1)");
        Ok("INSERT INTO t VALUES (4, 'a', NULL)");
        Ok("INSERT INTO t VALUES (5, 'c', 6)");
        // u: ids 1..4, w = id - 1 (t.k of the same id), m = w but NULL on id 2.
        Ok("CREATE TABLE u (id int64, w int64, m int64 NULL) BTREE");
        Ok("INSERT INTO u VALUES (0, 0)");
        Ok("INSERT INTO u VALUES (1, NULL)");
        Ok("INSERT INTO u VALUES (2, 2)");
        Ok("INSERT INTO u VALUES (3, 3)");
        // e: one row, x = 2.
        Ok("CREATE TABLE e (id int64, x int64) BTREE");
        Ok("INSERT INTO e VALUES (2)");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    std::string Ids(const std::string& where) { return Run("SELECT id FROM t WHERE " + where); }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The forms --------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, ACorrelatedExistsUnderOr) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k) OR sym = 'c'"),
              "id\\n1\\n2\\n3\\n4\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM u WHERE u.w = t.k))"), "id\\n5\\n6");
}

TEST_F(WhereBoolSubqueryTest, InAndNotInUnderNot) {
    EXPECT_EQ(Ids("NOT (k IN (SELECT w FROM u))"), "id\\n5\\n6");
    EXPECT_EQ(Ids("NOT (k NOT IN (SELECT w FROM u)) OR id = 6"), "id\\n1\\n2\\n3\\n4\\n6");
}

TEST_F(WhereBoolSubqueryTest, AScalarSubqueryUnderOr) {
    EXPECT_EQ(Ids("k = (SELECT w FROM u WHERE u.id = 2) OR id = 6"), "id\\n2\\n6");
    // No u row for t's 5 and 6: the scalar is NULL there, the comparison
    // unknown, and only the OR's other side can keep the row.
    EXPECT_EQ(Ids("k = (SELECT w FROM u WHERE u.id = t.id) OR sym = 'c'"),
              "id\\n1\\n2\\n3\\n4\\n6");
}

// ---- Hoisting -------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AFalseUncorrelatedExistsUnderOrEmptiesNothing) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM e WHERE x = 99) OR sym = 'c'"), "id\\n6");
    EXPECT_EQ(Ids("id = 1 OR EXISTS (SELECT id FROM e WHERE x = 99)"), "id\\n1");
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM e) OR id = 1"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM e WHERE x = 99))"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM e))"), "id");
}

TEST_F(WhereBoolSubqueryTest, AStatementRunTwiceAnswersTheSameWithItsTrail) {
    const std::string sql =
        "SELECT id FROM t WHERE id = 3 AND (EXISTS (SELECT id FROM e WHERE x = 99) OR sym = 'a')";
    EXPECT_EQ(Run(sql), "id\\n3");
    for (int i = 0; i < 4; ++i) EXPECT_EQ(Run(sql), "id\\n3");
}

// ---- Three-valued logic (BU-R4) -------------------------------------------

TEST_F(WhereBoolSubqueryTest, InOverASetHoldingNullIsUnknownAndNotKeepsItSo) {
    // u.m is {0, NULL, 2, 3}: `k IN` is true for k 0, 2, 3 and unknown for
    // the rest, so its NOT is false or unknown everywhere.
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u))"), "id");
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u))"), Ids("k NOT IN (SELECT m FROM u)"));
    EXPECT_EQ(Ids("k IN (SELECT m FROM u) OR id = 2"), "id\\n1\\n2\\n3\\n4");
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u)) OR n IS NULL"), "id\\n2\\n5");
}

// ---- Indexing ---------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, TwoSubqueriesInOneTree) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND u.id = 1) OR "
                  "k IN (SELECT w FROM u WHERE w > 2)"),
              "id\\n1\\n4");
}

TEST_F(WhereBoolSubqueryTest, TwoTreesOnOneStepReadTheirOwnSubqueries) {
    // The second tree's leaf is the step's second sub-chain: read as the
    // first (a false EXISTS), it would drop row 5.
    EXPECT_EQ(Ids("(id = 1 OR id = 5 OR EXISTS (SELECT id FROM e WHERE x = 99)) AND "
                  "(NOT EXISTS (SELECT id FROM u WHERE u.w = t.k) OR id = 1)"),
              "id\\n1\\n5");
}

TEST_F(WhereBoolSubqueryTest, ATreeBesideAFlatSubquery) {
    const std::string where =
        "EXISTS (SELECT id FROM u WHERE u.w = t.k) AND "
        "(k IN (SELECT w FROM u WHERE w = 0) OR k IN (SELECT w FROM u WHERE w = 3))";
    EXPECT_EQ(Ids(where), "id\\n1\\n4");
    // A write's WHERE attaches the flat sub-chain first, so the tree's leaves
    // are rebased past it.
    EXPECT_EQ(Run("DELETE FROM t WHERE " + where), "DELETED 2");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n5\\n6");
}

// ---- Placement --------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AcrossAJoinItWaitsForTheRelationItReads) {
    // The EXISTS reads u, the join's second step: placed at the first, it
    // would read a u row not yet bound.
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.k = u.w WHERE "
                  "EXISTS (SELECT id FROM e WHERE e.x = u.w) OR t.id > 3"),
              "t.id\\n3\\n4");
}

TEST_F(WhereBoolSubqueryTest, InsideASubqueryAndReachingTwoLevelsOut) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND "
                  "(EXISTS (SELECT id FROM e WHERE x = 7) OR u.id = 1))"),
              "id\\n1");
    // The innermost EXISTS reads t, two levels out: correlated, so never
    // answered once for the statement.
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND "
                  "(EXISTS (SELECT id FROM e WHERE e.x = t.k) OR u.id = 1))"),
              "id\\n1\\n3");
}

// ---- Writes -----------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AnUpdateAndADeleteWriteTheRowsTheSelectFinds) {
    EXPECT_EQ(Run("UPDATE t SET n = 0 WHERE EXISTS (SELECT id FROM e WHERE x = 99) OR id = 2"),
              "UPDATED 1");
    EXPECT_EQ(Ids("n = 0"), "id\\n2");
    EXPECT_EQ(Run("DELETE FROM t WHERE NOT (EXISTS (SELECT id FROM u WHERE u.w = t.k)) OR id = 1"),
              "DELETED 3");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n4");
}

// A write whose WHERE reads the relation it writes collects its targets
// before its first write, so a sub-chain re-run per row never reads the
// statement's own writes: each form below answered as if it did before.
const std::string kFirst = "NOT EXISTS (SELECT id FROM t AS x WHERE x.id < t.id)";

TEST_F(WhereBoolSubqueryTest, AWritesSubqueryOverItsOwnRelationSeesNoneOfItsWrites) {
    EXPECT_EQ(Ids(kFirst), "id\\n1");
    // Was DELETED 6: each delete made the next row the first.
    EXPECT_EQ(Run("DELETE FROM t WHERE id = 99 OR " + kFirst), "DELETED 1");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n4\\n5\\n6");
}

TEST_F(WhereBoolSubqueryTest, AFlatDeleteOverItsOwnRelationSeesNoneOfItsWrites) {
    EXPECT_EQ(Run("DELETE FROM t WHERE " + kFirst), "DELETED 1");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n4\\n5\\n6");
}

TEST_F(WhereBoolSubqueryTest, AnUncorrelatedDeleteOverItsOwnRelationSeesNoneOfItsWrites) {
    // Was DELETED 1: deleting id 1 made the EXISTS false for every later row.
    EXPECT_EQ(Run("DELETE FROM t WHERE EXISTS (SELECT id FROM t AS x WHERE x.id = 1)"),
              "DELETED 6");
    EXPECT_EQ(Ids("id > 0"), "id");
}

TEST_F(WhereBoolSubqueryTest, AnUpdateOverItsOwnRelationSeesNoneOfItsWrites) {
    // Was UPDATED 1: the first row's n = 7 made the NOT EXISTS false for the rest.
    EXPECT_EQ(Run("UPDATE t SET n = 7 WHERE NOT EXISTS "
                  "(SELECT id FROM t AS x WHERE x.id < t.id AND x.n = 7)"),
              "UPDATED 6");
    EXPECT_EQ(Ids("n = 7"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
}

// ---- The plan ---------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, APlanNamesTheSubqueryUnderItsLeaf) {
    const std::string plan =
        Run("ANALYZE SELECT id FROM t WHERE id = 1 OR EXISTS (SELECT id FROM u WHERE u.w = t.k)");
    EXPECT_NE(plan.find("filter bool (0:0.0 = 1 OR sub0)"), std::string::npos) << plan;
    EXPECT_NE(plan.find("\\n  sub0\\n    correlated EXISTS [correlated]"), std::string::npos)
        << plan;
}

// ---- No subquery under an exclusive hold ------------------------------------
//
// A write's WHERE subquery reads pages, and read under the write walk's
// exclusive leaf it is a held `X` asking an `S`: two statements on two cores
// close a cycle no latch wait breaks (`rules.md` §3's page latch row). So a
// write with one collects its targets first - in a read walk, or on a
// shared descent for a point write - and nothing reads the subquery's
// relation while a page of the written one is held exclusive. The store
// below sees every pin's mode, which is the hold an armed store latches.

class HoldWatchingStore final : public storage::PageStore {
public:
    explicit HoldWatchingStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        Watch(page_id);
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        Watch(page_id);
        return inner_.GetForReadUnpinned(page_id);
    }
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override {
        return inner_.RaiseAllocationFloor(first_allocatable_page_id);
    }
    void PinFrame(PageId page_id, storage::PinMode mode) noexcept override {
        pins_[page_id].push_back(mode);
    }
    // An unpin names no mode, so a page's pins are taken as released last
    // first - true of every hold these statements take on one page.
    void UnpinFrame(PageId page_id) noexcept override {
        auto it = pins_.find(page_id);
        if (it == pins_.end() || it->second.empty()) return;
        it->second.pop_back();
        if (it->second.empty()) pins_.erase(it);
    }

    // Every page a user relation owns fetched from now on is the watched
    // relation's. Told by the page's owner (page.md §2a), not by its id: a
    // catalog chain's growth pages share the user id range but carry a
    // catalog oid, below `kUserOidStart`.
    void Record(bool on) { recording_ = on; }
    // Fetches of a watched page while another page is pinned exclusive.
    int violations() const noexcept { return violations_; }

private:
    void Watch(PageId page_id) {
        if (recording_) {
            auto bytes = inner_.GetForReadUnpinned(page_id);
            if (bytes.ok() && storage::GetOwnerOid(bytes.value()) >= catalog::kUserOidStart) {
                watched_.insert(page_id);
            }
            return;
        }
        if (!watched_.contains(page_id)) return;
        for (const auto& [held, modes] : pins_) {
            if (watched_.contains(held)) continue;
            for (storage::PinMode mode : modes) {
                if (mode == storage::PinMode::kExclusive) ++violations_;
            }
        }
    }

    storage::InMemoryPageStore& inner_;
    std::unordered_map<PageId, std::vector<storage::PinMode>> pins_;
    std::unordered_set<PageId> watched_;
    bool recording_ = false;
    int violations_ = 0;
};

class WriteSubqueryHoldTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        // a: ids 1..4, k = id. b: ids 1..2, w = 2 and 4.
        Ok("CREATE TABLE a (id int64, k int64, v int64) BTREE");
        for (int k = 1; k <= 4; ++k) Ok("INSERT INTO a VALUES (" + std::to_string(k) + ", 0)");
        Ok("CREATE TABLE b (id int64, w int64) BTREE");
        Ok("INSERT INTO b VALUES (2)");
        Ok("INSERT INTO b VALUES (4)");
        store_.Record(true);
        Ok("SELECT id, w FROM b");
        store_.Record(false);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }

    storage::InMemoryPageStore inner_{kFirstUserPageId};
    HoldWatchingStore store_{inner_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

const std::string kInB = "EXISTS (SELECT id FROM b WHERE b.w = a.k)";

TEST_F(WriteSubqueryHoldTest, AWalkedUpdateReadsItsSubqueryUnderNoExclusiveHold) {
    EXPECT_EQ(Run("UPDATE a SET v = 1 WHERE " + kInB), "UPDATED 2");
    EXPECT_EQ(Run("SELECT id FROM a WHERE v = 1"), "id\\n2\\n4");
    EXPECT_EQ(store_.violations(), 0);
}

TEST_F(WriteSubqueryHoldTest, AWalkedDeleteReadsItsSubqueryUnderNoExclusiveHold) {
    EXPECT_EQ(Run("DELETE FROM a WHERE NOT " + kInB), "DELETED 2");
    EXPECT_EQ(Run("SELECT id FROM a"), "id\\n2\\n4");
    EXPECT_EQ(store_.violations(), 0);
}

// A tuple key beside a subquery is two conjuncts, so no point arm takes it:
// it is collected like any other, and a key that matches nothing - or names
// no row - writes nothing.
TEST_F(WriteSubqueryHoldTest, AKeyBesideASubqueryIsCollectedNotProbed) {
    EXPECT_EQ(Run("UPDATE a SET v = 1 WHERE id = 2 AND " + kInB), "UPDATED 1");
    EXPECT_EQ(Run("UPDATE a SET v = 1 WHERE id = 3 AND " + kInB), "UPDATED 0");
    EXPECT_EQ(Run("DELETE FROM a WHERE id = 4 AND " + kInB), "DELETED 1");
    EXPECT_EQ(Run("DELETE FROM a WHERE id = 9 AND " + kInB), "DELETED 0");
    EXPECT_EQ(Run("SELECT id, v FROM a"), "id,v\\n1,0\\n2,1\\n3,0");
    EXPECT_EQ(store_.violations(), 0);
}

}  // namespace
}  // namespace kds::server
