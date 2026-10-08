#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// **`PURGE` frees a deleted key** (BH,
// `instructions/v3.0.0/workorder-bh-purge-key.md`), through SQL, on one core.
//
// Three groups of cells:
//
//   - **Census B** (BH-S1, the order's §1.6): every consumer that keeps a
//     deleted row's pk - a secondary index, a Cabin, the foreign-key reverse
//     check through a Cabin, the inner build, an assertion's directory and a
//     Waystone trail - reads a key that was retired keyless and placed again
//     with different values as the new row, once. The retire is a test-only
//     seam (`RetireTombstone`), not the statement: the census asks how the
//     consumers read, not how the purge writes. **Expected green**, and a red
//     one stops BH for a ruling.
//   - **Census D**: what `DELETE` answers for a system relation, which PU10
//     says `PURGE` answers too.
//   - **Red at BH-S1** (committed red, as BD-S1's were): the statement
//     itself. BH-S3 turns it green.
//
// The waits - an older reader, an undecided deleter - are
// `lock_family_test.cpp`'s `PurgeWaitTest`, beside the lock family's other
// waits.

namespace kds::server {
namespace {

// The first field of every row a reply carries, in reply order: rows are
// comma-separated fields, separated by an escaped newline.
std::vector<std::uint64_t> Ids(const std::string& response) {
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < response.size();) {
        const std::size_t line_end = std::min(response.find("\\n", i), response.size());
        const std::size_t comma = response.find(',', i);
        if (comma != std::string::npos && comma < line_end) {
            const std::string digits = response.substr(i, comma - i);
            if (!digits.empty() &&
                std::all_of(digits.begin(), digits.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; })) {
                ids.push_back(std::stoull(digits));
            }
        }
        i = line_end + 2;
    }
    return ids;
}

std::size_t Count(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return static_cast<std::size_t>(std::count(ids.begin(), ids.end(), id));
}

bool Placed(const std::string& response) { return response.rfind("INSERTED", 0) == 0; }

// Every consumer Census B reads is wired: a transaction manager, the
// instance's one Cabin store, and a trail recorder with replay on.
class PurgeKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        recorder_.emplace(boot_->catalog, store_);
        d_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                   /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                   exec::Budget(), &*recorder_, /*replay_enabled=*/true,
                   /*access_statistics=*/true, &cabins_, &*mgr_);
    }

    std::string Run(const std::string& sql) { return d_->Dispatch(sql).response; }
    void Ok(const std::string& sql, const char* prefix) {
        const std::string out = Run(sql);
        ASSERT_EQ(out.rfind(prefix, 0), 0u) << sql << " -> " << out;
    }

    // **BH-S1's seam: a committed tombstone retired keyless, as BH-S2's
    // primitive will retire it** - under the leaf's exclusive hold, and
    // only a delete-marked slot - with neither its log record nor its
    // spill release, which Census B does not read.
    void RetireTombstone(const std::string& table, std::uint64_t pk) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        auto at = btree::BtreeLookup(store_, access.value()->desc_page_id, pk,
                                     storage::PageAccess::kWrite);
        ASSERT_TRUE(at.ok()) << "no slot keyed " << pk << ": " << at.status().message();
        heap::PageView leaf(at.value().leaf.bytes());
        auto tuple = leaf.ReadTuple(at.value().slot);
        ASSERT_TRUE(tuple.ok()) << tuple.status().message();
        ASSERT_TRUE(tuple.value().deleted) << "the seam retires tombstones only, and " << pk
                                           << " is live";
        ASSERT_TRUE(leaf.RetireSlot(at.value().slot).ok());
    }

    // `DELETE` of `pk`, the seam's retire, then `insert` naming `pk` again.
    void DeleteRetireAndPlaceAgain(const std::string& table, std::uint64_t pk,
                                   const std::string& insert) {
        Ok("DELETE FROM " + table + " WHERE id = " + std::to_string(pk), "DELETED 1");
        RetireTombstone(table, pk);
        const std::string placed = Run(insert);
        ASSERT_TRUE(Placed(placed)) << insert << " -> " << placed;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::CabinStore cabins_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> d_;
};

// ---- Census B: every consumer reads a re-placed key as the new row --------

TEST_F(PurgeKeyTest, CensusBAnIndexProbeAnswersTheReplacedKeyOnceUnderItsNewValue) {
    Ok("CREATE TABLE t (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE u (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE INDEX ix ON t (v)", "CREATED");
    for (int id : {1, 2, 3, 5}) Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 10)", "INSERTED");
    Ok("INSERT INTO u VALUES (1, 20)", "INSERTED");
    // The probe on the old value, before, so the index entry `(10, 5)` is
    // the one a stale reader would follow.
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE v = 10")), 5), 1u);

    DeleteRetireAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 20)");

    const std::string probe = Run("ANALYZE SELECT * FROM t WHERE v = 10");
    ASSERT_NE(probe.find("index_scanned="), std::string::npos) << "not an index probe: " << probe;
    EXPECT_EQ(Count(Ids(Run("SELECT * FROM t WHERE v = 10")), 5), 0u)
        << "the old key's entry answered the re-placed row";
    EXPECT_EQ(Ids(Run("SELECT * FROM t WHERE v = 20")), (std::vector<std::uint64_t>{5}));
    EXPECT_EQ(Count(Ids(Run("SELECT * FROM t WHERE v BETWEEN 0 AND 100")), 5), 1u)
        << "a range over both entries counted the row twice";
    // A join probing `t` by the index, from `u`'s row.
    const std::string joined = Run("SELECT t.id, u.id FROM u JOIN t ON t.v = u.v");
    EXPECT_EQ(Ids(joined), (std::vector<std::uint64_t>{5})) << joined;
}

TEST_F(PurgeKeyTest, CensusBACabinServesTheReplacedKeyOnceUnderItsNewValue) {
    Ok("CREATE TABLE t (id int64, sym varchar, qty int64) BTREE", "CREATED");
    Ok("CREATE CABIN ON t(sym)", "CREATED");
    for (int id : {1, 2, 5}) Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 'a', 1)", "INSERTED");
    // Observed, so the set for 'a' holds 5 when the key is re-placed.
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE sym = 'a'")), 5), 1u);
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE sym = 'a'")), 5), 1u);

    DeleteRetireAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 'b', 2)");

    // Served, not walked: the set for 'a' still names 5, which is the
    // entry this cell exists to read.
    const std::uint64_t hits_before = cabins_.stats().hits;
    EXPECT_EQ(Ids(Run("SELECT * FROM t WHERE sym = 'a'")), (std::vector<std::uint64_t>{1, 2}))
        << "the stale entry served the re-placed row under its old value";
    EXPECT_GT(cabins_.stats().hits, hits_before) << "the set for 'a' was not served";
    EXPECT_EQ(Ids(Run("SELECT * FROM t WHERE sym = 'b'")), (std::vector<std::uint64_t>{5}));
}

TEST_F(PurgeKeyTest, CensusBTheReverseCheckThroughACabinReadsTheReplacedChildsNewParent) {
    Ok("CREATE TABLE p (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE", "CREATED");
    Ok("CREATE CABIN ON c(pid)", "CREATED");
    Ok("INSERT INTO p VALUES (1, 0)", "INSERTED");
    Ok("INSERT INTO p VALUES (2, 0)", "INSERTED");
    Ok("INSERT INTO c VALUES (5, 1)", "INSERTED");
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM c WHERE pid = 1")), 5), 1u);
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM c WHERE pid = 1")), 5), 1u);

    DeleteRetireAndPlaceAgain("c", 5, "INSERT INTO c VALUES (5, 2)");

    // The set for pid = 1 still names child 5, which now references 2.
    const std::uint64_t hits_before = cabins_.stats().hits;
    const std::string freed = Run("DELETE FROM p WHERE id = 1");
    EXPECT_EQ(freed, "DELETED 1") << "a stale Cabin entry kept a parent with no child";
    EXPECT_GT(cabins_.stats().hits, hits_before) << "the reverse check walked, not the set";
    const std::string kept = Run("DELETE FROM p WHERE id = 2");
    EXPECT_EQ(kept.rfind("ERR FK_VIOLATION", 0), 0u) << "the re-placed child was not found: "
                                                     << kept;
}

TEST_F(PurgeKeyTest, CensusBTheInnerBuildBucketsTheReplacedRowUnderItsNewKey) {
    Ok("CREATE TABLE au (id int64, name varchar) BTREE", "CREATED");
    Ok("CREATE TABLE tr (id int64, au_id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO au VALUES (1, 'one')", "INSERTED");
    Ok("INSERT INTO au VALUES (2, 'two')", "INSERTED");
    for (int id : {3, 4, 5}) Ok("INSERT INTO tr VALUES (" + std::to_string(id) + ", 1, " + std::to_string(id) + ")", "INSERTED");

    DeleteRetireAndPlaceAgain("tr", 5, "INSERT INTO tr VALUES (5, 2, 50)");

    const std::string sql = "SELECT tr.id, au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id";
    const std::string plan = Run("ANALYZE " + sql);
    ASSERT_NE(plan.find("inner_built=1"), std::string::npos) << "the join did not build: " << plan;
    const std::string joined = Run(sql);
    EXPECT_EQ(Count(Ids(joined), 5), 1u) << joined;
    EXPECT_NE(joined.find("5,two,50"), std::string::npos) << joined;
    EXPECT_EQ(joined.find("5,one"), std::string::npos) << joined;
}

TEST_F(PurgeKeyTest, CensusBAnAssertionCountsAndSumsTheReplacedRowOnce) {
    // BH-R8's premise: `DELETE` wrote the departure, the retire writes none,
    // and the re-insert writes one arrival - so the group's aggregate is a
    // recount's, and the cap refuses the next row exactly as it should. A
    // second departure would leave room for it.
    struct Case {
        const char* check;
        const char* rows[2];  // the second is deleted, retired and placed again
        const char* over;
    };
    const Case cases[] = {
        {"COUNT(*) <= 2", {"(1, 7, 1)", "(2, 7, 1)"}, "(3, 7, 1)"},
        {"SUM(qty) <= 10", {"(1, 7, 6)", "(2, 7, 4)"}, "(3, 7, 1)"},
    };
    int n = 0;
    for (const Case& c : cases) {
        const std::string t = "trades" + std::to_string(n++);
        Ok("CREATE TABLE " + t + " (id int64, account int64, qty int64) BTREE", "CREATED");
        Ok("CREATE ASSERTION cap_" + t + " ON " + t + " GROUP BY (account) CHECK " + c.check,
           "CREATED");
        for (const char* row : c.rows) Ok("INSERT INTO " + t + " VALUES " + row, "INSERTED");

        DeleteRetireAndPlaceAgain(t, 2, "INSERT INTO " + t + " VALUES " + c.rows[1]);

        const std::string over = Run("INSERT INTO " + t + " VALUES " + c.over);
        EXPECT_EQ(over.rfind("ERR ASSERTION_VIOLATION", 0), 0u)
            << c.check << ": the next row was admitted: " << over;
        EXPECT_EQ(Ids(Run("SELECT * FROM " + t + " WHERE account = 7")),
                  (std::vector<std::uint64_t>{1, 2}))
            << c.check;
    }
}

TEST_F(PurgeKeyTest, CensusBAWaystoneReplayLandsWhereAFreshDescentWould) {
    Ok("CREATE TABLE t (id int64, v int64, label varchar) BTREE", "CREATED");
    for (int id : {1, 2, 5}) Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 10, 'x')", "INSERTED");
    // **A pk lookup, because only a lookup replays**: a trail never serves a
    // search (invariant 9, `IsTrailReplayable`), so a `WHERE v = ...` cell
    // would read no trail at all. The first run counts, the second records,
    // the third replays - and `replays=` is the evidence it did.
    const std::string keyed = "SELECT * FROM t WHERE id = 5";
    for (int i = 0; i < 3; ++i) ASSERT_EQ(Ids(Run(keyed)), (std::vector<std::uint64_t>{5}));
    const std::string before = Run("ANALYZE " + keyed);
    ASSERT_NE(before.find("replays="), std::string::npos) << "no trail to replay: " << before;

    DeleteRetireAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 20, 'y')");

    // The trail still names the retired slot. It is consulted - a replay or
    // a miss, whichever the slot now holds - and answers as a descent would.
    const std::string after = Run("ANALYZE " + keyed);
    EXPECT_TRUE(after.find("replays=") != std::string::npos ||
                after.find("trail_misses=") != std::string::npos)
        << "the trail was not consulted: " << after;
    const std::string row = Run(keyed);
    EXPECT_EQ(Ids(row), (std::vector<std::uint64_t>{5})) << row;
    EXPECT_NE(row.find("5,20,y"), std::string::npos)
        << "the trail's entry answered the re-placed key with its old row: " << row;
}

// ---- Census D: how `DELETE` refuses a system relation ---------------------

TEST_F(PurgeKeyTest, CensusDADeleteOfASystemRelationIsRefusedOnlyByAccident) {
    // PU10's second row asked what `DELETE` does with a system relation, so
    // `PURGE` could do the same. **It refuses nothing by design**: the name
    // resolves, and `InitTableAccess` fails because a bootstrap relation has
    // no `sys.columns` rows (`catalog.cpp`'s `"no columns for this rel_id"`).
    // That is not a refusal to follow, so BH-Q17 gives `PURGE` an explicit
    // one. This cell pins the accident, so a change to it is seen.
    const DispatchOutcome out = d_->Dispatch("DELETE FROM sys.tables WHERE id = 1");
    EXPECT_EQ(out.response, "ERR no columns for this rel_id");
    EXPECT_EQ(out.status.code(), StatusCode::kNotFound) << out.response;
}

// ---- Red at BH-S1: the statement ------------------------------------------

TEST_F(PurgeKeyTest, APurgedKeyIsNamedAgainAndPlaced) {
    // W2: "pk 정보 자체를 purge하는 다른 api ... 이 경우 재삽입이 가능함".
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1)", "INSERTED");
    Ok("INSERT INTO t VALUES (9, 1)", "INSERTED");
    Ok("DELETE FROM t WHERE id = 5", "DELETED 1");

    const DispatchOutcome purged = d_->Dispatch("PURGE FROM t WHERE id = 5");
    ASSERT_EQ(purged.response, "PURGED 1") << purged.response;
    EXPECT_EQ(purged.rows_affected, 1u);

    const std::string again = Run("INSERT INTO t VALUES (5, 2)");
    ASSERT_TRUE(Placed(again)) << again;
    EXPECT_EQ(Ids(Run("SELECT * FROM t")), (std::vector<std::uint64_t>{5, 9}));
    EXPECT_NE(Run("SELECT * FROM t WHERE id = 5").find("5,2"), std::string::npos);
}

}  // namespace
}  // namespace kds::server
