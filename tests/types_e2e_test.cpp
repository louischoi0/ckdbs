#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// TY07 - the new types end to end through the dispatcher
// (docs/workplan-types.md).
//
// **This task exists to fail if §1's claim is false.** The claim is that
// the four type primitives - a width, a literal parser, a comparison and a
// rendering - are all anything in this engine consumes, so a new type needs
// no new engine code anywhere above the codec. Every subsystem below
// therefore runs over `DATE`, `TIMESTAMP` and `DECIMAL` columns *without*
// any of them having been taught the types exist: the join, the fold, the
// Cabin's write hook and read path, the Waystone recorder and replay.
//
// A failure here is not a missing feature in this file. It is that claim
// being wrong, and the fix would be in the subsystem that turned out to
// need teaching.

namespace kds::server {
namespace {

// One database, one dispatcher, with whichever advisory structures the
// case under test needs.
class Instance {
public:
    explicit Instance(bool cabins = false, bool record = false, bool replay = false) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        if (cabins) cabins_.emplace();
        recorder_.emplace(boot_->catalog, store_);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), record ? &*recorder_ : nullptr, replay,
                            /*access_statistics=*/true, cabins_ ? &*cabins_ : nullptr);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

private:
    // `kFirstUserPageId`, never the default 1 - which is inside the
    // catalog's fixed pages and makes the third CREATE TABLE fail.
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> dispatcher_;
};

// A trade relation carrying all three new types, and an account relation to
// join it to. `settles` repeats across rows deliberately: a Cabin on a
// column whose every value is unique exercises nothing about entry sets,
// and a GROUP BY on one produces one group per row.
void Load(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE acct (id int64, name varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE trade (id int64, acct_id int64, settles date, "
                     "at timestamp, amt decimal(12, 2))")
                  .substr(0, 7),
              "CREATED");

    ASSERT_EQ(db.Run("INSERT INTO acct VALUES ('alice')").substr(0, 8), "INSERTED");
    ASSERT_EQ(db.Run("INSERT INTO acct VALUES ('bob')").substr(0, 8), "INSERTED");

    struct Row {
        const char* acct;
        const char* settles;
        const char* at;
        const char* amt;
    };
    // Written out rather than generated: the GROUP BY test asserts
    // first-seen group order, which is a property of *this* insertion
    // order and would be silently untested if the data were a loop.
    const Row rows[] = {
        {"1", "2026-03-02", "2026-03-01 09:00:00", "100.25"},
        {"2", "2026-03-02", "2026-03-01 10:30:00.500000", "200.75"},
        {"1", "2026-01-15", "2026-01-14 16:45:00", "50.00"},
        {"2", "2026-03-02", "2026-03-01 11:00:00", "10.00"},
        {"1", "2026-06-30", "2026-06-29 08:15:00", "999.99"},
    };
    for (const Row& r : rows) {
        const std::string sql = std::string("INSERT INTO trade VALUES (") + r.acct + ", '" +
                                r.settles + "', '" + r.at + "', '" + r.amt + "')";
        ASSERT_EQ(db.Run(sql).substr(0, 8), "INSERTED") << sql;
    }
}

// ---- INSERT / SELECT / UPDATE -------------------------------------------

TEST(TypesEndToEnd, ARowSurvivesInsertSelectAndUpdate) {
    Instance db;
    Load(db);

    EXPECT_EQ(db.Run("SELECT settles, at, amt FROM trade WHERE id = 1"),
              "settles,at,amt\\n2026-03-02,2026-03-01 09:00:00,100.25");

    // An UPDATE re-encodes every column its SET list did not touch, from
    // the decoded form - the round trip most likely to corrupt a
    // neighbouring value.
    ASSERT_EQ(db.Run("UPDATE trade SET amt = '111.11' WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(db.Run("SELECT settles, at, amt FROM trade WHERE id = 1"),
              "settles,at,amt\\n2026-03-02,2026-03-01 09:00:00,111.11");

    // And the reverse: updating a date leaves the decimal alone.
    ASSERT_EQ(db.Run("UPDATE trade SET settles = '2027-01-01' WHERE id = 1").substr(0, 7),
              "UPDATED");
    EXPECT_EQ(db.Run("SELECT settles, amt FROM trade WHERE id = 1"),
              "settles,amt\\n2027-01-01,111.11");
}

TEST(TypesEndToEnd, ADeletedRowStaysDeletedOnATypedPredicate) {
    Instance db;
    Load(db);
    ASSERT_EQ(db.Run("DELETE FROM trade WHERE settles = '2026-01-15'").substr(0, 7), "DELETED");
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE settles = '2026-01-15'"), "id");
}

// ---- Predicates ---------------------------------------------------------

TEST(TypesEndToEnd, ARangeOverDatesSelectsByCalendarOrder) {
    Instance db;
    Load(db);
    // Ordering on the encoded integer is ordering on the value, which is
    // the property that lets DATE reuse kInt everywhere.
    EXPECT_EQ(db.Run("SELECT id, settles FROM trade "
                     "WHERE settles BETWEEN '2026-02-01' AND '2026-05-01'"),
              "id,settles\\n1,2026-03-02\\n2,2026-03-02\\n4,2026-03-02");
}

TEST(TypesEndToEnd, ATimestampComparesBelowTheSecond) {
    Instance db;
    Load(db);
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE at < '2026-03-01 10:00:00'"), "id\\n1\\n3");
}

TEST(TypesEndToEnd, ADecimalPredicateMatchesOnTheScaledValue) {
    Instance db;
    Load(db);
    // '50' and '50.00' are the same value at scale 2 - the literal is
    // scaled at compile, so both find the row.
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt = '50.00'"), "id\\n3");
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt = '50'"), "id\\n3");
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt = 50"), "id\\n3");
}

// ---- Bare numeric literals (TY3 phase 2) --------------------------------

TEST(TypesEndToEnd, ABareNumericLiteralBehavesAsItsQuotedSpelling) {
    Instance db;
    Load(db);
    // The sugar rule, observed at the wire: every spelling of 50.00 finds
    // the same row, because the parser hands the compiler the same value.
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt = 50.00"), "id\\n3");
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt = 50.0"), "id\\n3");

    // INSERT and UPDATE take the bare form through the same encode gate.
    ASSERT_EQ(db.Run("INSERT INTO trade VALUES (2, '2026-07-01', "
                     "'2026-07-01 00:00:00', 12.34)")
                  .substr(0, 8),
              "INSERTED");
    EXPECT_EQ(db.Run("SELECT amt FROM trade WHERE settles = '2026-07-01'"), "amt\\n12.34");
    ASSERT_EQ(db.Run("UPDATE trade SET amt = 43.21 WHERE settles = '2026-07-01'").substr(0, 7),
              "UPDATED");
    EXPECT_EQ(db.Run("SELECT amt FROM trade WHERE settles = '2026-07-01'"), "amt\\n43.21");

    // A BETWEEN carries both bounds through the same coercion (the high
    // bound is the one a single-site fix misses - TY05's lesson).
    EXPECT_EQ(db.Run("SELECT id FROM trade WHERE amt BETWEEN 100.00 AND 300.00"), "id\\n1\\n2");
}

TEST(TypesEndToEnd, ABareNumericIsRefusedWhereItsQuotedSpellingWouldBe) {
    Instance db;
    Load(db);
    // Scale overflow is the same positioned compile error the quoted form
    // gets - one parser, whichever way the literal was spelled.
    const std::string overflow = db.Run("SELECT id FROM trade WHERE amt = 12.345");
    EXPECT_EQ(overflow.substr(0, 3), "ERR") << overflow;
    EXPECT_NE(overflow.find("12.345"), std::string::npos) << overflow;

    // Against an integer column, encode refuses it exactly as it refuses
    // the quoted string.
    const std::string bad = db.Run(
        "INSERT INTO trade VALUES (1.5, '2026-07-01', '2026-07-01 00:00:00', 12.34)");
    EXPECT_EQ(bad.substr(0, 3), "ERR") << bad;
    EXPECT_NE(bad.find("integer"), std::string::npos) << bad;

    // Against a varchar column it *stores the string* - the sugar rule has
    // no carve-out, and `'1.5'` into a varchar was always a plain string.
    ASSERT_EQ(db.Run("INSERT INTO acct VALUES (1.5)").substr(0, 8), "INSERTED");
    EXPECT_EQ(db.Run("SELECT name FROM acct WHERE name = '1.5'"), "name\\n1.5");

    // Against a DATE column, the shared date parser rejects the spelling.
    const std::string date = db.Run("SELECT id FROM trade WHERE settles = 1.5");
    EXPECT_EQ(date.substr(0, 3), "ERR") << date;
}

// ---- JOIN ---------------------------------------------------------------

TEST(TypesEndToEnd, AJoinCarriesTypedColumnsThroughTheFrame) {
    Instance db;
    Load(db);
    // The typed columns belong to the *second* step's relation, so this
    // also covers a value read through a chain frame at a non-zero slot.
    EXPECT_EQ(db.Run("SELECT a.name, t.settles, t.amt FROM acct AS a "
                     "JOIN trade AS t ON a.id = t.acct_id "
                     "WHERE t.settles = '2026-06-30'"),
              "a.name,t.settles,t.amt\\nalice,2026-06-30,999.99");
}

// ---- GROUP BY -----------------------------------------------------------

TEST(TypesEndToEnd, AggregatesOverADateKeyGroupFirstSeen) {
    Instance db;
    Load(db);
    // The done-when for this task: groups appear in the order their key was
    // first seen, and a DATE key renders as a date in the output.
    //
    // First-seen order over the insertion order above is 2026-03-02 (row
    // 1), then 2026-01-15 (row 3), then 2026-06-30 (row 5).
    EXPECT_EQ(db.Run("SELECT settles, COUNT(*), SUM(amt) FROM trade GROUP BY settles"),
              "settles,count(*),sum(amt)"
              "\\n2026-03-02,3,311.00"
              "\\n2026-01-15,1,50.00"
              "\\n2026-06-30,1,999.99");
}

TEST(TypesEndToEnd, SumOverDecimalIsExactAtScale) {
    Instance db;
    Load(db);
    // 100.25 + 200.75 + 50.00 + 10.00 + 999.99, summed as unscaled
    // integers and re-scaled once at the end - so no float rounding can
    // enter, which is the entire reason a financial engine has a DECIMAL.
    EXPECT_EQ(db.Run("SELECT SUM(amt) FROM trade"), "sum(amt)\\n1360.99");
}

TEST(TypesEndToEnd, AvgOverDecimalAnswersAtTheDeclaredScale) {
    Instance db;
    Load(db);
    // 1360.99 / 5 = 272.198 -> 272.20 at the column's scale 2, half-even
    // (feat-aggregate.md §3.4). No float touches the value: the quotient
    // is computed on the unscaled integers.
    EXPECT_EQ(db.Run("SELECT AVG(amt) FROM trade"), "avg(amt)\\n272.20");

    // Grouped, beside the exact halves it is computed from.
    EXPECT_EQ(db.Run("SELECT settles, AVG(amt), SUM(amt), COUNT(*) FROM trade "
                     "GROUP BY settles"),
              "settles,avg(amt),sum(amt),count(*)"
              "\\n2026-03-02,103.67,311.00,3"
              "\\n2026-01-15,50.00,50.00,1"
              "\\n2026-06-30,999.99,999.99,1");
}

TEST(TypesEndToEnd, AvgOverAnIntegerColumnIsRefusedWithTheHonestOptions) {
    Instance db;
    Load(db);
    // An integer column declared no scale, so there is no answer that
    // neither invents digits nor drops them - the refusal names the two
    // ways out (declare a DECIMAL, or compute SUM and COUNT).
    const std::string reply = db.Run("SELECT AVG(acct_id) FROM trade");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("decimal"), std::string::npos) << reply;
    EXPECT_NE(reply.find("SUM"), std::string::npos) << reply;
    EXPECT_NE(reply.find("at byte"), std::string::npos) << reply;

    const std::string date = db.Run("SELECT AVG(settles) FROM trade");
    EXPECT_EQ(date.substr(0, 3), "ERR") << date;
    EXPECT_NE(date.find("at byte"), std::string::npos) << date;
}

TEST(TypesEndToEnd, MinAndMaxOverEachNewTypeAreExact) {
    Instance db;
    Load(db);
    EXPECT_EQ(db.Run("SELECT MIN(settles), MAX(settles) FROM trade"),
              "min(settles),max(settles)\\n2026-01-15,2026-06-30");
    EXPECT_EQ(db.Run("SELECT MIN(at), MAX(at) FROM trade"),
              "min(at),max(at)\\n2026-01-14 16:45:00,2026-06-29 08:15:00");
    EXPECT_EQ(db.Run("SELECT MIN(amt), MAX(amt) FROM trade"),
              "min(amt),max(amt)\\n10.00,999.99");
}

// ---- Cabin --------------------------------------------------------------

TEST(TypesEndToEnd, ACabinOnADateColumnRecordsServesAndDrops) {
    // A Cabin's key is built from the decoded value, so a DATE column keys
    // on the epoch integer and a DECIMAL on (unscaled, scale). What this
    // pins is the contract suite's central property, over a typed column:
    // **the answer does not depend on whether a Cabin exists.**
    Instance plain;
    Load(plain);
    Instance cabined(/*cabins=*/true);
    Load(cabined);

    ASSERT_EQ(cabined.Run("CREATE CABIN ON trade(settles)").substr(0, 7), "CREATED");

    const char* queries[] = {
        "SELECT id FROM trade WHERE settles = '2026-03-02'",
        "SELECT id FROM trade WHERE settles = '2026-01-15'",
        // A value no row has: the case only a Cabin can answer without
        // opening the relation, and the one where a wrong answer is
        // invisible without a baseline to compare against.
        "SELECT id FROM trade WHERE settles = '2030-12-25'",
    };

    // Twice, because observation is n=1 for a declared Cabin but the second
    // pass is the one served from it - the first records while walking.
    for (int pass = 0; pass < 2; ++pass) {
        for (const char* q : queries) {
            EXPECT_EQ(cabined.Run(q), plain.Run(q)) << "pass " << pass << ": " << q;
        }
    }

    // Writes after observation - §5's witness. An append must reach the
    // observed value's set, or the next read loses a row.
    ASSERT_EQ(cabined.Run("INSERT INTO trade VALUES (1, '2026-03-02', "
                          "'2026-03-02 12:00:00', '1.00')")
                  .substr(0, 8),
              "INSERTED");
    ASSERT_EQ(plain.Run("INSERT INTO trade VALUES (1, '2026-03-02', "
                        "'2026-03-02 12:00:00', '1.00')")
                  .substr(0, 8),
              "INSERTED");
    for (const char* q : queries) {
        EXPECT_EQ(cabined.Run(q), plain.Run(q)) << "after the write: " << q;
    }

    // And dropping it changes nothing, which is what makes an authoritative
    // structure safely evictable (§1's corollary).
    ASSERT_EQ(cabined.Run("DROP CABIN ON trade(settles)").substr(0, 7), "DROPPED");
    for (const char* q : queries) {
        EXPECT_EQ(cabined.Run(q), plain.Run(q)) << "after the drop: " << q;
    }
}

// ---- Waystone -----------------------------------------------------------

TEST(TypesEndToEnd, ATrailOverADecimalResidualReplaysIdentically) {
    // Invariant 8, over a chain whose residual is a decimal comparison:
    // turning replay on must not change a reply. The pk equality is what
    // makes the step lookup-class and therefore replayable; the decimal
    // residual rides along on the same step, which is exactly the shape the
    // done-when asks for.
    Instance off(/*cabins=*/false, /*record=*/false, /*replay=*/false);
    Load(off);
    Instance on(/*cabins=*/false, /*record=*/true, /*replay=*/true);
    Load(on);

    const char* queries[] = {
        "SELECT id, amt FROM trade WHERE id = 2 AND amt = '200.75'",
        "SELECT id, settles FROM trade WHERE id = 5 AND settles = '2026-06-30'",
        // The same shape with a value that does not match - a recorded
        // trail must not turn a non-match into a row.
        "SELECT id, amt FROM trade WHERE id = 2 AND amt = '0.01'",
    };

    // Three passes: the first counts, the second records, the third is the
    // one served from the trail (recording is n=2 for observed traffic).
    for (int pass = 0; pass < 3; ++pass) {
        for (const char* q : queries) {
            EXPECT_EQ(on.Run(q), off.Run(q)) << "pass " << pass << ": " << q;
        }
    }
}

// ---- Refusals reach the wire with their positions ------------------------

TEST(TypesEndToEnd, SumOverADateIsRefusedThroughTheWire) {
    Instance db;
    Load(db);
    const std::string reply = db.Run("SELECT SUM(settles) FROM trade");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("at byte"), std::string::npos) << reply;
}

TEST(TypesEndToEnd, AMixedScaleComparisonIsRefusedThroughTheWire) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE money (id int64, two decimal(10, 2), three decimal(10, 3))")
                  .substr(0, 7),
              "CREATED");
    const std::string reply = db.Run("SELECT id FROM money WHERE two = three");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("rescale"), std::string::npos) << reply;
}

TEST(TypesEndToEnd, ABadLiteralIsRefusedThroughTheWireWithItsByte) {
    Instance db;
    Load(db);
    const std::string reply = db.Run("SELECT id FROM trade WHERE settles = '2026-02-30'");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("at byte"), std::string::npos) << reply;
}

TEST(TypesEndToEnd, AnOutOfRangeInsertIsRefusedAndWritesNothing) {
    Instance db;
    Load(db);
    const std::string before = db.Run("SELECT COUNT(*) FROM trade");
    EXPECT_EQ(db.Run("INSERT INTO trade VALUES (1, '1850-01-01', "
                     "'2026-03-01 09:00:00', '1.00')")
                  .substr(0, 3),
              "ERR");
    EXPECT_EQ(db.Run("SELECT COUNT(*) FROM trade"), before);
}

}  // namespace
}  // namespace kds::server
