#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "act_on_fetch_store.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/role.hpp"
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
//   - **The consumers** (BH-S1's Census B, the order's §1.6, run through the
//     statement since BH-S3): every structure that keeps a deleted row's pk
//     - a secondary index, a Cabin, the foreign-key reverse check through a
//     Cabin, the inner build, an assertion's directory and a Waystone trail
//     - reads a key that was purged and placed again with different values
//     as the new row, once. The assertion cell is BH-R8's: `PURGE` writes
//     no second departure.
//   - **Census D**: what `DELETE` answers for a system relation, which
//     `PURGE` does not follow (BH-Q17).
//   - **The statement** (PU1-PU12, BH-R11's SQL cells): a key purged and
//     placed again, every refusal in PU10 with its code and byte, the
//     window's edges.
//
// The waits - an older reader, an undecided deleter - are
// `lock_family_test.cpp`'s `PurgeWaitTest`, beside the lock family's other
// waits; the storage and crash cells are `purge_key_crash_test.cpp`'s.

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

    // `DELETE` of `pk`, its `PURGE`, then `insert` naming `pk` again.
    void DeletePurgeAndPlaceAgain(const std::string& table, std::uint64_t pk,
                                  const std::string& insert) {
        const std::string where = " WHERE id = " + std::to_string(pk);
        Ok("DELETE FROM " + table + where, "DELETED 1");
        Ok("PURGE FROM " + table + where, "PURGED 1");
        const std::string placed = Run(insert);
        ASSERT_TRUE(Placed(placed)) << insert << " -> " << placed;
    }

    // A refusal's code and the byte its text names.
    void Refused(const std::string& sql, StatusCode code, std::uint32_t byte) {
        const DispatchOutcome out = d_->Dispatch(sql);
        EXPECT_EQ(out.status.code(), code) << sql << " -> " << out.response;
        EXPECT_NE(out.response.find("(byte " + std::to_string(byte) + ")"), std::string::npos)
            << sql << " -> " << out.response;
    }

    // Every fetch through a store that can refuse one (`FailFetch`), for the
    // refusal-while-writing cell.
    storage::InMemoryPageStore backing_{kFirstUserPageId};
    testing_race::ActOnFetchStore store_{backing_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::CabinStore cabins_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> d_;
};

// ---- The consumers: each reads a purged and re-placed key as the new row ---

TEST_F(PurgeKeyTest, CensusBAnIndexProbeAnswersTheReplacedKeyOnceUnderItsNewValue) {
    Ok("CREATE TABLE t (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE u (id int64, v int64) BTREE", "CREATED");
    Ok("CREATE INDEX ix ON t (v)", "CREATED");
    for (int id : {1, 2, 3, 5}) {
        Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 10)", "INSERTED");
    }
    Ok("INSERT INTO u VALUES (1, 20)", "INSERTED");
    // The probe on the old value, before, so the index entry `(10, 5)` is
    // the one a stale reader would follow.
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE v = 10")), 5), 1u);

    DeletePurgeAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 20)");

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
    for (int id : {1, 2, 5}) {
        Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 'a', 1)", "INSERTED");
    }
    // Observed, so the set for 'a' holds 5 when the key is re-placed.
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE sym = 'a'")), 5), 1u);
    ASSERT_EQ(Count(Ids(Run("SELECT * FROM t WHERE sym = 'a'")), 5), 1u);

    DeletePurgeAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 'b', 2)");

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

    DeletePurgeAndPlaceAgain("c", 5, "INSERT INTO c VALUES (5, 2)");

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
    for (int id : {3, 4, 5}) {
        const std::string k = std::to_string(id);
        Ok("INSERT INTO tr VALUES (" + k + ", 1, " + k + ")", "INSERTED");
    }

    DeletePurgeAndPlaceAgain("tr", 5, "INSERT INTO tr VALUES (5, 2, 50)");

    const std::string sql = "SELECT tr.id, au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id";
    const std::string plan = Run("ANALYZE " + sql);
    ASSERT_NE(plan.find("inner_built=1"), std::string::npos) << "the join did not build: " << plan;
    const std::string joined = Run(sql);
    EXPECT_EQ(Count(Ids(joined), 5), 1u) << joined;
    EXPECT_NE(joined.find("5,two,50"), std::string::npos) << joined;
    EXPECT_EQ(joined.find("5,one"), std::string::npos) << joined;
}

TEST_F(PurgeKeyTest, CensusBAnAssertionCountsAndSumsTheReplacedRowOnce) {
    // BH-R8's premise: `DELETE` wrote the departure, the purge writes none,
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

        DeletePurgeAndPlaceAgain(t, 2, "INSERT INTO " + t + " VALUES " + c.rows[1]);

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
    for (int id : {1, 2, 5}) {
        Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 10, 'x')", "INSERTED");
    }
    // **A pk lookup, because only a lookup replays**: a trail never serves a
    // search (invariant 9, `IsTrailReplayable`), so a `WHERE v = ...` cell
    // would read no trail at all. The first run counts, the second records,
    // the third replays - and `replays=` is the evidence it did.
    const std::string keyed = "SELECT * FROM t WHERE id = 5";
    for (int i = 0; i < 3; ++i) ASSERT_EQ(Ids(Run(keyed)), (std::vector<std::uint64_t>{5}));
    const std::string before = Run("ANALYZE " + keyed);
    ASSERT_NE(before.find("replays="), std::string::npos) << "no trail to replay: " << before;

    DeletePurgeAndPlaceAgain("t", 5, "INSERT INTO t VALUES (5, 20, 'y')");

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
    // one (`EveryRefusalCarriesItsCodeAndByte` below). This cell pins the
    // accident, so a change to it is seen.
    const DispatchOutcome out = d_->Dispatch("DELETE FROM sys.tables WHERE id = 1");
    EXPECT_EQ(out.response, "ERR no columns for this rel_id");
    EXPECT_EQ(out.status.code(), StatusCode::kNotFound) << out.response;
}

// ---- The statement ---------------------------------------------------------

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

TEST_F(PurgeKeyTest, AnAbsentKeyPurgesNothingAndARunIsIdempotent) {
    // PU3: never placed, or already purged - neither is a refusal, so a
    // `PURGE` can be run again.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1)", "INSERTED");
    Ok("DELETE FROM t WHERE id = 5", "DELETED 1");
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 7"), "PURGED 0");
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 5"), "PURGED 1");
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 5"), "PURGED 0");
    // A window outside the key space names no key.
    EXPECT_EQ(Run("PURGE FROM t WHERE id > 1099511627775"), "PURGED 0");
    EXPECT_EQ(Run("PURGE FROM t WHERE id < 1"), "PURGED 0");
}

TEST_F(PurgeKeyTest, AWindowPurgesEveryDeletedKeyAndPassesOverLiveOnes) {
    // BH-Q2 (b): a window wider than one key passes over a live key.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    for (int id = 1; id <= 9; ++id) {
        Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)", "INSERTED");
    }
    for (int id : {2, 4, 6, 8}) {
        Ok("DELETE FROM t WHERE id = " + std::to_string(id), "DELETED 1");
    }
    const DispatchOutcome purged = d_->Dispatch("PURGE FROM t WHERE id >= 3 AND id <= 8");
    EXPECT_EQ(purged.response, "PURGED 3") << "4, 6 and 8, and nothing live";
    EXPECT_EQ(purged.rows_affected, 3u);
    EXPECT_EQ(Ids(Run("SELECT * FROM t")), (std::vector<std::uint64_t>{1, 3, 5, 7, 9}));
    EXPECT_EQ(Run("INSERT INTO t VALUES (2, 1)").rfind("ERR", 0), 0u)
        << "2 lay outside the window and was freed";
    for (int id : {4, 6, 8}) {
        EXPECT_TRUE(Placed(Run("INSERT INTO t VALUES (" + std::to_string(id) + ", 2)"))) << id;
    }
    EXPECT_EQ(Run("PURGE FROM t WHERE id BETWEEN 1 AND 2"), "PURGED 1");
}

TEST_F(PurgeKeyTest, EveryKeyOfAWhollyPurgedLeafIsPlacedAgain) {
    // BH's close measurement found it (`bench/v3.0.0/results-bh-close-*`):
    // a non-first leaf whose every slot was purged is full of retired slots
    // and holds no key, so naming its low key again split it as an append
    // and promoted a separator its parent already held - refused, for good.
    // A full leaf with retired slots is compacted first (BH-S5).
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    std::string load = "INSERT INTO t VALUES ";
    for (int id = 1; id <= 1000; ++id) {
        load += "(" + std::to_string(id) + ", 1)" + (id < 1000 ? ", " : "");
    }
    Ok(load, "INSERTED");
    Ok("DELETE FROM t WHERE id >= 1 AND id <= 1000", "DELETED 1000");
    Ok("PURGE FROM t WHERE id BETWEEN 1 AND 1000", "PURGED 1000");
    for (int id = 1; id <= 1000; ++id) {
        const std::string again = Run("INSERT INTO t VALUES (" + std::to_string(id) + ", 2)");
        ASSERT_TRUE(Placed(again)) << id << ": " << again;
    }
    const std::vector<std::uint64_t> walked = Ids(Run("SELECT * FROM t"));
    ASSERT_EQ(walked.size(), 1000u);
    EXPECT_TRUE(std::is_sorted(walked.begin(), walked.end()));
    for (int id : {1, 167, 333, 500, 834, 1000}) {
        EXPECT_EQ(Ids(Run("SELECT * FROM t WHERE id = " + std::to_string(id))),
                  (std::vector<std::uint64_t>{static_cast<std::uint64_t>(id)}));
    }
}

TEST_F(PurgeKeyTest, APartlyPurgedLeafTakesKeysBelowItsLastOneDescending) {
    // The other shape a purge leaves: a full leaf with one keyed slot among
    // retired ones. A key below it sorts inside the leaf, so it would divide
    // - and a divide needs two keys, so it was refused `OutOfSpace`. The
    // compaction takes it, and the keys go back in descending order.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    std::string load = "INSERT INTO t VALUES ";
    for (int id = 1; id <= 600; ++id) {
        load += "(" + std::to_string(id) + ", 1)" + (id < 600 ? ", " : "");
    }
    Ok(load, "INSERTED");
    // The second leaf's keys, read off the tree: `[low, high]`.
    auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    auto first = btree::BtreeLeftmostLeaf(store_, access.value()->desc_page_id);
    ASSERT_TRUE(first.ok());
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    {
        auto page = store_.GetForRead(first.value());
        ASSERT_TRUE(page.ok());
        const PageId second = heap::PageView(page.value().bytes()).next_page_id();
        ASSERT_NE(second, kInvalidPageId);
        auto leaf = store_.GetForRead(second);
        ASSERT_TRUE(leaf.ok());
        low = heap::PageView(leaf.value().bytes()).min_key();
        const PageId third = heap::PageView(leaf.value().bytes()).next_page_id();
        ASSERT_NE(third, kInvalidPageId);
        auto next = store_.GetForRead(third);
        ASSERT_TRUE(next.ok());
        high = heap::PageView(next.value().bytes()).min_key() - 1;
    }
    // Every key of it deleted and purged but its last.
    const std::string window =
        " WHERE id BETWEEN " + std::to_string(low) + " AND " + std::to_string(high - 1);
    Ok("DELETE FROM t" + window, "DELETED");
    Ok("PURGE FROM t" + window, "PURGED");
    for (std::uint64_t id = high - 1; id >= low; --id) {
        const std::string again = Run("INSERT INTO t VALUES (" + std::to_string(id) + ", 2)");
        ASSERT_TRUE(Placed(again)) << id << ": " << again;
    }
    const std::vector<std::uint64_t> walked = Ids(Run("SELECT * FROM t"));
    EXPECT_EQ(walked.size(), 600u);
    EXPECT_TRUE(std::is_sorted(walked.begin(), walked.end()));
}

TEST_F(PurgeKeyTest, ALiveKeyIsRefusedInAWindowOfOneHoweverItIsSpelled) {
    // PU3, BH-Q2 (b): `id = 5` and `id BETWEEN 5 AND 5` are one statement.
    // `PURGE FROM t WHERE id = 5` puts the literal at byte 24.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1)", "INSERTED");
    Refused("PURGE FROM t WHERE id = 5", StatusCode::kInvalidArgument, 24);
    Refused("PURGE FROM t WHERE id BETWEEN 5 AND 5", StatusCode::kInvalidArgument, 30);
    Refused("PURGE FROM t WHERE id >= 5 AND id < 6", StatusCode::kInvalidArgument, 25);
    EXPECT_EQ(Ids(Run("SELECT * FROM t")), (std::vector<std::uint64_t>{5}));
}

TEST_F(PurgeKeyTest, AConjunctTheWindowCannotReadIsRefusedNotSkipped) {
    // BH-R2: skipping `!=` would widen the window - `id > 0 AND id != 5`
    // would free 5.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1)", "INSERTED");
    Ok("DELETE FROM t WHERE id = 5", "DELETED 1");
    Refused("PURGE FROM t WHERE id > 0 AND id != 5", StatusCode::kNotImplemented, 30);
    const std::string again = Run("INSERT INTO t VALUES (5, 2)");
    EXPECT_NE(again.find("PURGE frees its key"), std::string::npos)
        << "5 is still the tombstone, and the duplicate text names the way out: " << again;
}

TEST_F(PurgeKeyTest, EveryRefusalCarriesItsCodeAndByte) {
    // PU10's table, every row a statement reaches on one core with no
    // concurrent writer. The bound's row is `PurgeWaitTest`'s.
    Ok("CREATE TABLE t (id int64, qty int64, ts timestamp NULL) BTREE", "CREATED");
    Ok("CREATE TABLE h (id int64, qty int64) HEAP", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1, NULL)", "INSERTED");
    Ok("DELETE FROM t WHERE id = 5", "DELETED 1");

    struct Case {
        const char* sql;
        StatusCode code;
        std::uint32_t byte;
    };
    const Case cases[] = {
        {"PURGE FROM tables WHERE id = 1", StatusCode::kUnsupported, 11},
        {"PURGE FROM sys.tables WHERE id = 1", StatusCode::kUnsupported, 11},
        {"PURGE FROM h WHERE id = 1", StatusCode::kUnsupported, 11},
        {"PURGE FROM t", StatusCode::kNotImplemented, 12},
        {"PURGE FROM t WHERE qty = 1", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE id != 5", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE ts IS NULL", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE id = qty", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE id IN (SELECT id FROM t)", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE EXISTS (SELECT id FROM t)", StatusCode::kNotImplemented, 13},
        {"PURGE FROM t WHERE DATE(ts) = DATE(ts)", StatusCode::kNotImplemented, 19},
        {"PURGE FROM t WHERE id = -7", StatusCode::kInvalidArgument, 24},
        {"PURGE FROM t WHERE id BETWEEN 1 AND -7", StatusCode::kInvalidArgument, 36},
        // Past int64 the lexer wraps: 2^64 is 0 and 2^64 + 5 is 5, so a fold of
        // the wrapped value would free every deleted key, or the wrong one
        // (BH-S5's review).
        {"PURGE FROM t WHERE id >= 18446744073709551616", StatusCode::kInvalidArgument, 25},
        {"PURGE FROM t WHERE id = 18446744073709551621", StatusCode::kInvalidArgument, 24},
        {"PURGE FROM t WHERE id BETWEEN 1 AND 18446744073709551621",
         StatusCode::kInvalidArgument, 36},
    };
    for (const Case& c : cases) Refused(c.sql, c.code, c.byte);
    // None of them purged 5.
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 5"), "PURGED 1");
}

TEST_F(PurgeKeyTest, ARefusalWhileWritingKeepsTheKeysItPurgedAndSaysHowMany) {
    // PU6, PU10's "refusal while writing": the window was judged whole, and
    // the refusal comes from a key's own write - here the var-heap page
    // refusing the second key's spill release, after that key's retire. Each
    // key before it is purged whole, the refused one is purged with its
    // spill leaked, and the text says both; the rest are untouched, and a
    // re-run finishes the window.
    Ok("CREATE TABLE t (id int64, s varchar(16)) BTREE", "CREATED");
    const std::string spilled(100, 'x');
    for (int id : {1, 2, 3}) {
        Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", '" + spilled + "')", "INSERTED");
        Ok("DELETE FROM t WHERE id = " + std::to_string(id), "DELETED 1");
    }
    auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    store_.FailFetch(access.value()->varheap_page_id, 2);

    const DispatchOutcome refused = d_->Dispatch("PURGE FROM t WHERE id BETWEEN 1 AND 3");
    EXPECT_EQ(refused.status.code(), StatusCode::kIoError) << refused.response;
    EXPECT_NE(refused.response.find("primary key 2 is purged"), std::string::npos)
        << refused.response;
    EXPECT_NE(refused.response.find("purged 1 key(s) of its window before it"), std::string::npos)
        << refused.response;

    EXPECT_TRUE(Placed(Run("INSERT INTO t VALUES (1, 'a')")));
    EXPECT_TRUE(Placed(Run("INSERT INTO t VALUES (2, 'b')")));
    EXPECT_NE(Run("INSERT INTO t VALUES (3, 'c')").find("PURGE frees its key"), std::string::npos)
        << "a key the refused purge never reached was freed";
    EXPECT_EQ(Run("PURGE FROM t WHERE id BETWEEN 1 AND 3"), "PURGED 1");
}

TEST_F(PurgeKeyTest, InsideATransactionItIsRefusedAndPoisonsNothing) {
    // PU9, BH-Q4 (a): refused before the transaction is touched.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (5, 1)", "INSERTED");
    Ok("DELETE FROM t WHERE id = 5", "DELETED 1");
    Session s;
    ASSERT_EQ(d_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome refused = d_->Dispatch("PURGE FROM t WHERE id = 5", &s);
    EXPECT_EQ(refused.status.code(), StatusCode::kNotImplemented) << refused.response;
    EXPECT_NE(refused.response.find("(byte 0)"), std::string::npos) << refused.response;
    EXPECT_TRUE(Placed(d_->Dispatch("INSERT INTO t VALUES (6, 1)", &s).response))
        << "the refusal poisoned the transaction";
    EXPECT_EQ(d_->Dispatch("COMMIT", &s).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(Ids(Run("SELECT * FROM t")), (std::vector<std::uint64_t>{6}));
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 5"), "PURGED 1");
}

TEST_F(PurgeKeyTest, AReadWriteSessionIsRefusedAsAnyUnclassifiedStatementIs) {
    // BH-Q10 (a): `PURGE` is admin's, `RequiredRole`'s unclassified default.
    Ok("CREATE TABLE t (id int64, qty int64) BTREE", "CREATED");
    Session writer;
    writer.set_role(Role::kReadWrite);
    const std::string delete_reply = d_->Dispatch("DELETE FROM t WHERE id = 1", &writer).response;
    ASSERT_EQ(delete_reply.rfind("DELETED", 0), 0u) << delete_reply;
    const std::string purge_reply = d_->Dispatch("PURGE FROM t WHERE id = 1", &writer).response;
    EXPECT_EQ(purge_reply.rfind("ERR", 0), 0u) << purge_reply;
    EXPECT_NE(purge_reply.find("admin"), std::string::npos) << purge_reply;
}

}  // namespace
}  // namespace kds::server
