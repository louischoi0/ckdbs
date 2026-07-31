#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The read-path integration (waystone-concpets.md section 3.1, spec test
// 12-3 in its amended form).
//
// One property dominates this file: **the answer does not depend on
// Waystone.** Every test that checks a fast-path result checks it against
// the same query with Waystone off, because the amendment's whole safety
// argument is that the probe chooses where to look and never what is
// visible. A test that only asserted "the probe is used" would pass just
// as happily on an implementation that returned the wrong row.

namespace kds::server {
namespace {

class WaystoneSelectTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        d_.emplace(boot_->superblock, boot_->catalog, store_);

        ASSERT_EQ(Run("CREATE TABLE t (id int64, v int32, txt varchar)").substr(0, 7), "CREATED");
        // A twin with Waystone off, for the equivalence comparison. It
        // cannot be the same relation disabled and re-enabled: enabling is
        // refused once a relation holds rows, because backfill does not
        // exist. Same schema, same insert order, so the same per-relation
        // id sequence - every row of `t` has an identical row in `u`.
        ASSERT_EQ(Run("CREATE TABLE u (id int64, v int32, txt varchar)").substr(0, 7), "CREATED");
    }

    std::string Run(const std::string& line) { return d_->Dispatch(line).response; }

    void Enable() { ASSERT_EQ(Run("WAYSTONE ENABLE t").substr(0, 2), "OK"); }
    void Disable() { ASSERT_EQ(Run("WAYSTONE DISABLE t").substr(0, 2), "OK"); }

    // Inserts the same n rows into both tables, so a query against either
    // must produce identical bytes.
    void InsertRows(int n) {
        for (const char* table : {"t", "u"}) {
            for (int i = 0; i < n; ++i) {
                const std::string reply =
                    Run(std::string("INSERT INTO ") + table + " VALUES (" +
                        std::to_string(i * 10) + ", 'r" + std::to_string(i) + "')");
                ASSERT_EQ(reply.substr(0, 8), "INSERTED") << reply;
            }
        }
    }

    // The same query against the Waystone-enabled table and its twin.
    // Byte-identical is the assertion; anything weaker is not equivalence,
    // and equivalence is the entire safety argument for the amendment.
    void ExpectSameWithAndWithout(const std::string& query_on_t) {
        std::string query_on_u = query_on_t;
        const std::size_t at = query_on_u.find(" t ");
        ASSERT_NE(at, std::string::npos) << query_on_t;
        query_on_u.replace(at, 3, " u ");

        const std::string with = Run(query_on_t);
        const std::string without = Run(query_on_u);
        EXPECT_EQ(with, without) << query_on_t;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> d_;
};

// ---- Equivalence, which is the whole contract ---------------------------

TEST_F(WaystoneSelectTest, EveryPkLookupAgreesWithTheScan) {
    Enable();
    InsertRows(40);
    for (int pk = 1; pk <= 40; ++pk) {
        ExpectSameWithAndWithout("SELECT * FROM t WHERE id = " + std::to_string(pk));
    }
}

TEST_F(WaystoneSelectTest, APkThatDoesNotExistAgreesWithTheScan) {
    Enable();
    InsertRows(5);
    // A miss must produce the header line and no rows, exactly as the scan
    // does - not an error, and not a stale row from a zeroed entry.
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id = 999");
    EXPECT_EQ(Run("SELECT * FROM t WHERE id = 999"), "id,v,txt");
}

TEST_F(WaystoneSelectTest, DeletedAndOutOfRangePksAgreeWithTheScan) {
    Enable();
    InsertRows(3);
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id = 0");
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id = 4");
}

TEST_F(WaystoneSelectTest, QueriesTheFastPathDeclinesStillWork) {
    Enable();
    InsertRows(10);
    // None of these is a bare pk equality, so all must fall through - and
    // must be unaffected by having done so.
    ExpectSameWithAndWithout("SELECT * FROM t ");
    ExpectSameWithAndWithout("SELECT * FROM t WHERE v = 30");
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id = 2 AND v = 10");
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id > 5");
}

TEST_F(WaystoneSelectTest, ResultsAreUnchangedAfterTheDirectoryIsDropped) {
    // Spec section 12-3: deleting the structure wholesale costs performance
    // and changes no result. DISABLE drops the root reference, which is the
    // observable half of deleting its pages.
    Enable();
    InsertRows(20);
    std::vector<std::string> before;
    for (int pk = 1; pk <= 20; ++pk) {
        before.push_back(Run("SELECT * FROM t WHERE id = " + std::to_string(pk)));
    }

    Disable();
    for (int pk = 1; pk <= 20; ++pk) {
        EXPECT_EQ(Run("SELECT * FROM t WHERE id = " + std::to_string(pk)), before[pk - 1]);
    }
}

TEST_F(WaystoneSelectTest, AStaleEntryPointingAtAnotherRowDoesNotProduceItsRow) {
    // The failure the Keystone-id check exists to stop, forced by hand:
    // rewrite pk 1's entry to name pk 2's slot, with a matching epoch so
    // the epoch check cannot catch it. Without rule 2 this returns row 2
    // for a query asking about row 1.
    Enable();
    InsertRows(3);
    const std::string truth = Run("SELECT * FROM t WHERE id = 1");
    const std::string other = Run("SELECT * FROM t WHERE id = 2");
    ASSERT_NE(truth, other);

    auto access = boot_->catalog.InitTableAccess(
        boot_->catalog.FindTableOidByName("t").value());
    ASSERT_TRUE(access.ok());
    const stats::WaystoneRef ws{access.value()->waystone_dir_root,
                                access.value()->waystone_dir_depth};

    auto two = stats::LookupEntry(store_, ws, 2);
    ASSERT_TRUE(two.ok());
    auto leaf = stats::LookupEntryPage(store_, ws.dir_root, ws.depth, 1);
    ASSERT_TRUE(leaf.ok());
    auto bytes = store_.Get(leaf.value());
    ASSERT_TRUE(bytes.ok());
    auto one = stats::ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()),
                               stats::EntrySlotOf(1));
    ASSERT_TRUE(one.ok());

    stats::WaystoneEntry poisoned = one.value();
    poisoned.page_id = two.value().page_id;
    poisoned.slot = two.value().slot;  // now names the tuple for pk 2
    ASSERT_TRUE(stats::WriteEntry(bytes.value(), stats::EntrySlotOf(1), poisoned).ok());

    EXPECT_EQ(Run("SELECT * FROM t WHERE id = 1"), truth)
        << "a stale entry must fall through to the scan, not return the row it points at";
}

// ---- Coverage is maintained by INSERT -----------------------------------

TEST_F(WaystoneSelectTest, RowsInsertedAfterEnablingAreAllProbeable) {
    Enable();
    InsertRows(30);
    const std::string status = Run("WAYSTONE STATUS t");

    for (int pk = 1; pk <= 30; ++pk) {
        ASSERT_NE(Run("SELECT * FROM t WHERE id = " + std::to_string(pk)), "id,v,txt")
            << "pk " << pk << " was not found; coverage is incomplete";
    }
    // Every one of those 30 was a probe hit, not a fallback.
    EXPECT_NE(Run("WAYSTONE STATUS t").find("probe_misses=0"), std::string::npos) << status;
}

TEST_F(WaystoneSelectTest, RowsInsertedBeforeEnablingAreNotCoveredSoEnablingIsRefused) {
    // Backfill (T17) does not exist. Silently accepting would leave a
    // kCovered relation whose entries cover only part of it, and a reader
    // entitled to treat a miss as "no such row" would then be wrong.
    InsertRows(3);
    const std::string reply = Run("WAYSTONE ENABLE t");
    EXPECT_EQ(reply.substr(0, 4), "ERR ") << reply;
    EXPECT_NE(reply.find("backfill"), std::string::npos) << reply;
}

// ---- The command surface ------------------------------------------------

TEST_F(WaystoneSelectTest, EnablingTwiceIsIdempotent) {
    Enable();
    EXPECT_EQ(Run("WAYSTONE ENABLE t"), "OK already enabled");
}

TEST_F(WaystoneSelectTest, DisablingThenReEnablingWorksAndGetsAFreshDirectory) {
    Enable();
    const std::string first = Run("WAYSTONE STATUS t");
    Disable();
    EXPECT_NE(Run("WAYSTONE STATUS t").find("state=0"), std::string::npos);

    ASSERT_EQ(Run("WAYSTONE ENABLE t").substr(0, 2), "OK");
    EXPECT_NE(Run("WAYSTONE STATUS t"), first) << "re-enabling allocates a new root";
}

TEST_F(WaystoneSelectTest, StatusWithoutATableReportsOnlyTheCounters) {
    const std::string reply = Run("WAYSTONE STATUS");
    EXPECT_NE(reply.find("probe_hits="), std::string::npos);
    EXPECT_EQ(reply.find("state="), std::string::npos);
}

TEST_F(WaystoneSelectTest, BadWaystoneCommandsAreRejectedNotIgnored) {
    EXPECT_EQ(Run("WAYSTONE").substr(0, 4), "ERR ");
    EXPECT_EQ(Run("WAYSTONE ENABLE").substr(0, 4), "ERR ");
    EXPECT_EQ(Run("WAYSTONE ENABLE nosuchtable").substr(0, 4), "ERR ");
    EXPECT_EQ(Run("WAYSTONE FROBNICATE t").substr(0, 4), "ERR ");
}

TEST_F(WaystoneSelectTest, ADisabledRelationDoesNoWaystoneWorkAtAll) {
    // Spec section 12-6: a relation with Waystone off pays nothing. The
    // page count is the only visible proxy - no directory, no entry pages.
    InsertRows(10);
    const std::size_t pages = store_.page_count();
    for (int pk = 1; pk <= 10; ++pk) {
        Run("SELECT * FROM t WHERE id = " + std::to_string(pk));
    }
    EXPECT_EQ(store_.page_count(), pages);
    EXPECT_NE(Run("WAYSTONE STATUS").find("probe_hits=0"), std::string::npos);
}


// ---- UPDATE takes the same fast path, under the same contract -----------

TEST_F(WaystoneSelectTest, PointUpdateByPkAgreesWithTheScannedUpdate) {
    Enable();
    InsertRows(20);

    // Same statement against both tables; both must report one row and
    // leave the same row behind.
    for (int pk = 1; pk <= 20; ++pk) {
        const std::string set = " SET v = " + std::to_string(pk * 7) +
                                " WHERE id = " + std::to_string(pk);
        EXPECT_EQ(Run("UPDATE t" + set), "UPDATED 1") << pk;
        EXPECT_EQ(Run("UPDATE u" + set), "UPDATED 1") << pk;
        ExpectSameWithAndWithout("SELECT * FROM t WHERE id = " + std::to_string(pk));
    }
}

TEST_F(WaystoneSelectTest, PointUpdateOfAMissingPkTouchesNothing) {
    Enable();
    InsertRows(3);
    EXPECT_EQ(Run("UPDATE t SET v = 1 WHERE id = 999"), "UPDATED 0");
    EXPECT_EQ(Run("UPDATE u SET v = 1 WHERE id = 999"), "UPDATED 0");
}

TEST_F(WaystoneSelectTest, UpdatesTheFastPathDeclinesStillWork) {
    Enable();
    InsertRows(10);
    // A non-pk predicate matches by value, so it must still scan and may
    // touch several rows.
    EXPECT_EQ(Run("UPDATE t SET v = 99 WHERE v = 30"), Run("UPDATE u SET v = 99 WHERE v = 30"));
    // An extra AND cannot be evaluated by a probe.
    EXPECT_EQ(Run("UPDATE t SET v = 5 WHERE id = 2 AND v = 10"),
              Run("UPDATE u SET v = 5 WHERE id = 2 AND v = 10"));
    // No WHERE at all updates everything.
    EXPECT_EQ(Run("UPDATE t SET v = 7"), Run("UPDATE u SET v = 7"));
    ExpectSameWithAndWithout("SELECT * FROM t ");
}

TEST_F(WaystoneSelectTest, APointUpdateStillRefusesToChangeThePk) {
    Enable();
    InsertRows(2);
    const std::string reply = Run("UPDATE t SET id = 5 WHERE id = 1");
    EXPECT_EQ(reply.substr(0, 4), "ERR ") << reply;
    // Rejected before any storage is touched, probe path or not.
    ExpectSameWithAndWithout("SELECT * FROM t WHERE id = 1");
}

TEST_F(WaystoneSelectTest, AProbedUpdateDoesNotDisturbTheWaystoneEntry) {
    // UPDATE is in place, so the tuple does not move and its entry stays
    // valid. A subsequent probe must still hit rather than having been
    // invalidated by the write.
    Enable();
    InsertRows(5);
    const std::string before = Run("WAYSTONE STATUS t");
    ASSERT_EQ(Run("UPDATE t SET v = 42 WHERE id = 3"), "UPDATED 1");

    EXPECT_NE(Run("SELECT * FROM t WHERE id = 3").find("42"), std::string::npos);
    EXPECT_NE(Run("WAYSTONE STATUS t").find("probe_misses=0"), std::string::npos) << before;
}

}  // namespace
}  // namespace kds::server
