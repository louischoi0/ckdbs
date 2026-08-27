#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/txn/manager.hpp"

// **The index contract** (docs/spec/index.md §1, workplan IX12), modelled on
// `waystone_contract_test.cpp` and holding an index to a *higher* bar than
// that file holds a trail to.
//
// A trail is advisory: invariant 8 lets it be stale, lossy and collision-prone
// precisely because none of it can reach an answer. An index is
// **authoritative** - it is consulted for what exists, not merely for where to
// look - so it cannot be deleted wholesale without changing a reply. What it
// can be held to is the other half of the same standard: **an accelerator may
// cost performance and must never change a query result.**
//
// So the same query set runs over the same data in four configurations and
// every reply is compared byte for byte:
//
//   1. indexed, filled by the write hook   the ordinary one
//   2. indexed, filled by the backfill     declared after the rows existed
//   3. `indexes = off`                     the index exists and is ignored
//   4. no index at all                     the baseline
//
// Configuration 2 is what says the backfill and the write hook agree about
// what an entry is; 3 and 4 together say the index changed neither the plan
// nor the answer.
//
// Four cases get their own tests because a shared query set cannot express
// them: an old snapshot reading through a superseded entry, a delete whose
// entry survives, two string keys sharing a truncated prefix, and a
// deliberately corrupted index page - the last being the one that proves the
// page's own checked widths are load-bearing rather than decorative.

namespace kds::server {
namespace {

// One database at one configuration. An optional transaction manager,
// because the snapshot case needs real undo and the rest does not care.
class Instance {
public:
    explicit Instance(bool indexes) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_,
                            txn::IsolationLevel::kReadCommitted, /*core_id=*/0, indexes);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// The schema every configuration runs over. `sym` is a string so the
// truncated key path is exercised by the shared set too, and `qty` gives the
// composite index a second column.
void Schema(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, owner int64, qty int64, sym varchar) BTREE")
                  .substr(0, 7),
              "CREATED");
}

void Rows(Instance& db) {
    for (int i = 0; i < 40; ++i) {
        const std::string owner = std::to_string(i % 5);
        const std::string qty = std::to_string(i % 4);
        const std::string sym = "'s" + std::to_string(i % 3) + "'";
        ASSERT_EQ(db.Run("INSERT INTO b VALUES (" + owner + ", " + qty + ", " + sym + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
    // Writes that exercise the append-only rules: a key that moves, a key
    // that comes back, a covered column that moves, and a delete whose entry
    // survives.
    ASSERT_EQ(db.Run("UPDATE b SET owner = 9 WHERE id = 3").substr(0, 7), "UPDATED");
    ASSERT_EQ(db.Run("UPDATE b SET owner = 0 WHERE id = 4").substr(0, 7), "UPDATED");
    ASSERT_EQ(db.Run("UPDATE b SET qty = 7 WHERE id = 5").substr(0, 7), "UPDATED");
    ASSERT_EQ(db.Run("DELETE FROM b WHERE id = 6").substr(0, 7), "DELETED");
}

void Indexes(Instance& db) {
    ASSERT_EQ(db.Run("CREATE INDEX by_owner ON b (owner) COVERING (qty)").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE INDEX by_owner_qty ON b (owner, qty)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE INDEX by_sym ON b (sym)").substr(0, 7), "CREATED");
}

// 1 and 3: declared first, so the write hook fills them.
void LoadMaintained(Instance& db) {
    Schema(db);
    Indexes(db);
    Rows(db);
}

// 2: declared last, so the backfill fills them - including from the undo
// chain, which is what makes it comparable at all.
void LoadBackfilled(Instance& db) {
    Schema(db);
    Rows(db);
    Indexes(db);
}

// 4: the baseline.
void LoadPlain(Instance& db) {
    Schema(db);
    Rows(db);
}

const std::vector<std::string>& Queries() {
    static const std::vector<std::string> kQueries = {
        // Probes: a hit, a moved key, a key that came back, a miss.
        "SELECT id, owner, qty FROM b WHERE owner = 1",
        "SELECT id FROM b WHERE owner = 9",
        "SELECT id FROM b WHERE owner = 0",
        "SELECT id FROM b WHERE owner = 42",
        // The composite index, entered by one column and by two.
        "SELECT id FROM b WHERE owner = 2",
        "SELECT id FROM b WHERE owner = 2 AND qty = 2",
        // The covering filter: a predicate the entry can decide.
        "SELECT id FROM b WHERE owner = 1 AND qty = 3",
        // Ranges.
        "SELECT id FROM b WHERE owner BETWEEN 1 AND 3",
        "SELECT id FROM b WHERE owner BETWEEN 8 AND 10",
        // The string key, which is stored truncated.
        "SELECT id FROM b WHERE sym = 's1'",
        "SELECT id FROM b WHERE sym = 'nope'",
        // The deleted row must be absent whichever way it is reached.
        "SELECT id FROM b WHERE owner = 1 AND qty = 1",
        // A pk predicate, which no index may take over.
        "SELECT id FROM b WHERE id = 3",
        // Folds, because a fold collapses rows to a number and so catches a
        // duplicate or a dropped row that a projection would print plainly.
        "SELECT COUNT(*) FROM b WHERE owner = 1",
        "SELECT COUNT(*), MIN(qty), MAX(qty) FROM b WHERE owner = 2",
        "SELECT owner, COUNT(*) FROM b GROUP BY owner",
        // And the whole relation, which every configuration must agree on.
        "SELECT id, owner, qty, sym FROM b",

        // ---- Paginated statements (docs/spec/parser-v2.md I11, V09) ---------
        //
        // A limited index step must slice, never reorder: the walk
        // collects in index-key order and emission is pk order (§8a), so
        // the quota keeps the first rows *by pk* - the same rows every
        // other configuration keeps. The multi-match probe under LIMIT 1
        // is the case where stopping the collect phase early would drop a
        // pk-earlier row; these lines pin that it never does, in all four
        // configurations at once.
        "SELECT id FROM b WHERE owner = 1 LIMIT 1",
        "SELECT id FROM b WHERE owner = 1 LIMIT 2 OFFSET 1",
        "SELECT id FROM b WHERE owner = 2 AND qty = 2 LIMIT 1",
        "SELECT id FROM b WHERE owner BETWEEN 1 AND 3 LIMIT 3",
        "SELECT id FROM b WHERE owner = 42 LIMIT 1",
        "SELECT id, owner, qty, sym FROM b LIMIT 5 OFFSET 2",

        // ---- The correlated probe (spec §8a, IX17) ---------------------
        //
        // A self-join whose inner side is keyed by the outer row - no
        // literal for propagation to reach, so with the index declared the
        // inner step is the per-outer-row IndexProbe, and under
        // `indexes = off` and in `plain` it is the walk. All four must
        // agree byte for byte, the same equivalence §1 demands of every
        // other kind.
        "SELECT a.id, c.id FROM b AS a JOIN b AS c ON c.owner = a.id "
        "WHERE a.id BETWEEN 1 AND 4",
        "SELECT a.id FROM b AS a JOIN b AS c ON c.owner = a.id "
        "WHERE a.id BETWEEN 1 AND 4 LIMIT 3",
        "SELECT id FROM b AS o WHERE EXISTS "
        "(SELECT i.id FROM b AS i WHERE i.owner = o.id)",
    };
    return kQueries;
}

// ---- §1: the four configurations agree, byte for byte -------------------

TEST(IndexContractTest, EveryConfigurationReturnsTheSameBytes) {
    Instance maintained(/*indexes=*/true);
    Instance backfilled(/*indexes=*/true);
    Instance ignored(/*indexes=*/false);
    Instance plain(/*indexes=*/true);

    LoadMaintained(maintained);
    LoadBackfilled(backfilled);
    LoadMaintained(ignored);
    LoadPlain(plain);

    for (const std::string& sql : Queries()) {
        const std::string expected = plain.Run(sql);
        EXPECT_EQ(maintained.Run(sql), expected) << "write-hook-filled index: " << sql;
        EXPECT_EQ(backfilled.Run(sql), expected) << "backfilled index: " << sql;
        EXPECT_EQ(ignored.Run(sql), expected) << "indexes = off: " << sql;
    }
}

TEST(IndexContractTest, TheIndexedRunActuallyUsedTheIndex) {
    // The control every equivalence suite needs: without it, all four
    // configurations could be agreeing because none of them used an index.
    Instance db(/*indexes=*/true);
    LoadMaintained(db);

    const std::string plan = db.Run("ANALYZE SELECT id FROM b WHERE owner = 1");
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("index_scanned="), std::string::npos) << plan;
    EXPECT_EQ(plan.find("examined=39"), std::string::npos)
        << "the indexed run read the whole relation: " << plan;
}

// The value of `key=<n>` in an ANALYZE reply.
std::uint64_t MeterOf(const std::string& reply, const std::string& key) {
    const auto at = reply.find(key + "=");
    EXPECT_NE(at, std::string::npos) << key << " not in: " << reply;
    if (at == std::string::npos) return 0;
    return std::strtoull(reply.c_str() + at + key.size() + 1, nullptr, 10);
}

TEST(IndexContractTest, AFilledQuotaStopsResolvingIndexEntries) {
    // V09's quota against the two-phase step, and the asymmetry is the
    // point. The *collect* phase may never stop early - index-key order is
    // not pk order, so a shortened collect could drop a pk-earlier row -
    // but each *resolve* is a base descent, and a sink that stopped ends
    // that loop on the row that filled the quota. `index_resolved=` is the
    // witness the saving is real; the paginated query-set lines above are
    // the witness the reply is still the right slice.
    Instance db(/*indexes=*/true);
    LoadMaintained(db);

    const std::string full = db.Run("ANALYZE SELECT id FROM b WHERE owner = 1");
    const std::string limited = db.Run("ANALYZE SELECT id FROM b WHERE owner = 1 LIMIT 1");

    ASSERT_GE(MeterOf(full, "index_resolved"), 2u)
        << "the fixture must multi-match to prove anything";
    EXPECT_EQ(MeterOf(limited, "index_resolved"), 1u) << limited;
    EXPECT_EQ(MeterOf(limited, "index_scanned"), MeterOf(full, "index_scanned"))
        << "the collect phase must not shorten - it is what keeps the slice a pk-order slice";
}

// ---- MVCC: an old snapshot reads through a superseded entry --------------

TEST(IndexContractTest, AnOldSnapshotFindsItsVersionThroughTheStaleEntry) {
    // The case removal would break, and the reason §2 calls it *incorrect*
    // rather than merely unnecessary. A repeatable-read reader holds a view
    // from before the update; the entry naming the old key is exactly what
    // it must reach its version through.
    Instance db(/*indexes=*/true);
    LoadMaintained(db);

    Session reader;
    ASSERT_EQ(db.Run(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ").substr(0, 5), "BEGIN");
    const std::string before = db.Run(reader, "SELECT id FROM b WHERE owner = 1");

    // A second session moves a row out of owner = 1.
    db.Ok("UPDATE b SET owner = 77 WHERE id = 2");

    EXPECT_EQ(db.Run(reader, "SELECT id FROM b WHERE owner = 1"), before)
        << "the old snapshot lost a row when the key moved under it";
    // ...and does not see the row under its new key either.
    EXPECT_EQ(db.Run(reader, "SELECT id FROM b WHERE owner = 77"),
              db.Run(reader, "SELECT id FROM b WHERE owner = 12345"));
    ASSERT_EQ(db.Run(reader, "COMMIT").substr(0, 6), "COMMIT");

    // A fresh reader sees the move.
    EXPECT_NE(db.Run("SELECT id FROM b WHERE owner = 77").find('2'), std::string::npos);
}

TEST(IndexContractTest, ADeletedRowsEntrySurvivesAndStillReturnsNothing) {
    // DELETE does not touch the index (removal is forbidden), so the only
    // thing keeping the row out of the answer is the visibility predicate -
    // which an index step reaches through AcceptTupleAt like every other
    // kind.
    Instance db(/*indexes=*/true);
    Instance plain(/*indexes=*/true);
    Schema(db);
    Indexes(db);
    Schema(plain);
    for (Instance* i : {&db, &plain}) {
        ASSERT_EQ(i->Run("INSERT INTO b VALUES (5, 1, 'a')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("INSERT INTO b VALUES (5, 2, 'b')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("DELETE FROM b WHERE id = 1").substr(0, 7), "DELETED");
    }

    EXPECT_EQ(db.Run("SELECT id FROM b WHERE owner = 5"),
              plain.Run("SELECT id FROM b WHERE owner = 5"));
    // The entry is still there - that is the point.
    EXPECT_NE(db.Run("SHOW INDEXES").find("entries=2"), std::string::npos)
        << db.Run("SHOW INDEXES");
}

// ---- Truncation: two keys sharing a prefix -------------------------------

TEST(IndexContractTest, TwoStringKeysSharingATruncatedPrefixStayDistinct) {
    // A string key is stored truncated (spec §6), so these two encode
    // identically. That is a *false positive* the read-time key re-check
    // subtracts - and this is the test that says the re-check is real rather
    // than assumed.
    Instance db(/*indexes=*/true);
    Instance plain(/*indexes=*/true);
    const std::string base(exec::kIndexStringKeyBytes, 'x');

    for (Instance* i : {&db, &plain}) {
        ASSERT_EQ(i->Run("CREATE TABLE t (id int64, sym varchar) BTREE").substr(0, 7), "CREATED");
    }
    ASSERT_EQ(db.Run("CREATE INDEX ix ON t (sym)").substr(0, 7), "CREATED");
    for (Instance* i : {&db, &plain}) {
        ASSERT_EQ(i->Run("INSERT INTO t VALUES ('" + base + "aaa')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("INSERT INTO t VALUES ('" + base + "zzz')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("INSERT INTO t VALUES ('" + base + "aaa')").substr(0, 8), "INSERTED");
    }

    for (const std::string& sql :
         {"SELECT id FROM t WHERE sym = '" + base + "aaa'",
          "SELECT id FROM t WHERE sym = '" + base + "zzz'",
          "SELECT id FROM t WHERE sym = '" + base + "qqq'"}) {
        EXPECT_EQ(db.Run(sql), plain.Run(sql)) << sql;
    }

    // And the index really was consulted - all three entries share one key,
    // so the probe scans three and returns one.
    const std::string plan = db.Run("ANALYZE SELECT id FROM t WHERE sym = '" + base + "zzz'");
    EXPECT_NE(plan.find("index_scanned=3"), std::string::npos) << plan;
    EXPECT_NE(plan.find("matched=1"), std::string::npos) << plan;
}

TEST(IndexContractTest, ASpilledCoveredValueKeepsItsRowRatherThanDecidingIt) {
    // The conservative direction of the covering filter, and the one a
    // mutation test found unexercised. A covered value longer than the
    // inline cell lives in the var-heap, so the entry carries a *pointer*
    // and not the bytes - and resolving one would be a page fetch under the
    // index leaf's span, which I15's R1 forbids. So the entry cannot decide
    // and the row must be kept for the base read to filter.
    //
    // Wrong in this direction costs a wasted descent. Wrong in the other
    // costs a row, silently, and only for values long enough to spill.
    Instance db(/*indexes=*/true);
    Instance plain(/*indexes=*/true);
    const std::string long_a(200, 'a');
    const std::string long_b(200, 'b');

    for (Instance* i : {&db, &plain}) {
        ASSERT_EQ(i->Run("CREATE TABLE t (id int64, k int64, note varchar) BTREE").substr(0, 7),
                  "CREATED");
    }
    ASSERT_EQ(db.Run("CREATE INDEX ix ON t (k) COVERING (note)").substr(0, 7), "CREATED");
    for (Instance* i : {&db, &plain}) {
        ASSERT_EQ(i->Run("INSERT INTO t VALUES (1, '" + long_a + "')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("INSERT INTO t VALUES (1, '" + long_b + "')").substr(0, 8), "INSERTED");
        ASSERT_EQ(i->Run("INSERT INTO t VALUES (1, 'short')").substr(0, 8), "INSERTED");
    }

    const std::vector<std::string> kChecks = {
        "SELECT id FROM t WHERE k = 1 AND note = '" + long_a + "'",
        "SELECT id FROM t WHERE k = 1 AND note = 'short'",
        "SELECT id FROM t WHERE k = 1 AND note = 'absent'",
        "SELECT id, note FROM t WHERE k = 1"};
    for (const std::string& sql : kChecks) {
        EXPECT_EQ(db.Run(sql), plain.Run(sql)) << sql;
    }

    // The index was used, and the spilled rows were *not* filtered out of
    // it - one short value could be decided, the two long ones could not.
    const std::string plan =
        db.Run("ANALYZE SELECT id FROM t WHERE k = 1 AND note = '" + long_a + "'");
    EXPECT_NE(plan.find("index_scanned=3"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("index_filtered=2"), std::string::npos)
        << "a spilled covered value was decided from the entry: " << plan;
}

// ---- Damage: a corrupted index page must fail, not mis-answer ------------

TEST(IndexContractTest, ACorruptedIndexPageFailsRatherThanReturningWrongRows) {
    // The counterpart of the waystone suite's corrupted-trail case, and it
    // has to come out the other way. A trail is advisory, so damage is a
    // miss and the statement falls through; an index is authoritative, so
    // damage has no fall-through that could be correct - it must be an
    // error. Returning *plausible* rows is the one outcome neither structure
    // may produce.
    Instance db(/*indexes=*/true);
    LoadMaintained(db);
    ASSERT_NE(db.Run("SELECT id FROM b WHERE owner = 1").rfind("ERR", 0), 0u);

    auto row = db.catalog().FindIndexByName("by_owner");
    ASSERT_TRUE(row.ok()) << row.status().message();

    // Claim more entries than the page can hold. A build that trusted the
    // count would read past the array and answer from whatever was there.
    auto page = db.store().Get(row.value().root_page_id);
    ASSERT_TRUE(page.ok());
    const std::uint16_t absurd = 60000;
    std::memcpy(page.value().bytes().data() + index::kIndexHeaderOffset +
                    index::kIndexLeafNrEntriesOffset,
                &absurd, sizeof(absurd));

    const std::string out = db.Run("SELECT id FROM b WHERE owner = 1");
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << "a corrupted index answered a query: " << out;
}

TEST(IndexContractTest, AnIndexWhoseWidthsDisagreeWithItsCatalogRowIsCorruption) {
    // The checked half of index_page.hpp's redundancy, reached through a
    // statement rather than through the storage layer - which is where it
    // has to hold, because that is where a wrong answer would come out.
    Instance db(/*indexes=*/true);
    LoadMaintained(db);

    auto row = db.catalog().FindIndexByName("by_sym");
    ASSERT_TRUE(row.ok());
    auto page = db.store().Get(row.value().root_page_id);
    ASSERT_TRUE(page.ok());
    const std::uint16_t wrong = 3;
    std::memcpy(page.value().bytes().data() + index::kIndexHeaderOffset +
                    index::kIndexLeafKeyWidthOffset,
                &wrong, sizeof(wrong));

    const std::string out = db.Run("SELECT id FROM b WHERE sym = 's1'");
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
}

TEST(IndexContractTest, TwoIndexStepsInOneChainDoNotShareOnePhaseTwoBuffer) {
    // **The nested case the shared query set cannot express**, because every
    // statement in it opens one relation. A join whose *both* sides are
    // served by an index puts two index steps in one chain, and the inner
    // one runs inside the outer one's phase-2 loop - so the runner-held
    // scratch that carries phase 1's pks to phase 2 is live on two frames at
    // once. Sharing it made the outer step resolve the inner step's pks and
    // drop eleven of thirteen rows: the index changed the reply, which is
    // the one thing §1 does not allow it to do.
    Instance on(/*indexes=*/true);
    Instance off(/*indexes=*/false);
    LoadMaintained(on);
    LoadMaintained(off);

    const std::string sql =
        "SELECT a.id, c.id FROM b AS a JOIN b AS c ON a.qty = c.qty "
        "WHERE a.owner = 1 AND c.owner = 2";
    EXPECT_EQ(on.Run(sql), off.Run(sql));
}

}  // namespace
}  // namespace kds::server
